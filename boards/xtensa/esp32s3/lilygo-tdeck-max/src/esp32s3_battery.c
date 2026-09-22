/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_battery.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * The T-Deck Max runs from a single 1400 mAh cell.  A BQ27220 fuel gauge
 * measures it and an SY6970 charges it from USB, both on the shared I2C
 * bus.  Both chips are powered by the cell rather than by the ESP32-S3, so
 * they keep their settings across resets of the board, but not across the
 * battery being disconnected; the settings are therefore applied at every
 * boot.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>
#include <nuttx/wqueue.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/power/battery_charger.h>
#include <nuttx/power/battery_gauge.h>
#include <nuttx/power/bq27220.h>
#include <nuttx/power/sy6970.h>

#include "esp32s3_i2c.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif
#ifdef CONFIG_ESP32S3_USBSERIAL
#  include "esp32s3_usbserial.h"
#endif

#include "lilygo-tdeck-max.h"

#if defined(CONFIG_BQ27220) || defined(CONFIG_SY6970)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TDECKMAX_BATTERY_I2C_FREQUENCY 400000

/* How often to check for USB power.  This is how long a USB cable plugged
 * into a sleeping board takes to be noticed; the check itself wakes the
 * chip for about a millisecond.
 */

#define TDECKMAX_USB_POLL_MS           2000

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_gauge_initialize
 *
 * Description:
 *   This is where the gauge learns the size of the cell.  It only takes
 *   time when the gauge had it wrong: a couple of seconds, once.
 *
 ****************************************************************************/

#ifdef CONFIG_BQ27220
static int tdeckmax_gauge_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct battery_gauge_dev_s *gauge;

  gauge = bq27220_initialize(i2c, BQ27220_I2C_ADDRESS,
                             TDECKMAX_BATTERY_I2C_FREQUENCY,
                             CONFIG_LILYGO_TDECK_MAX_BATTERY_CAPACITY);
  if (gauge == NULL)
    {
      return -ENODEV;
    }

  return battery_gauge_register("/dev/batt0", gauge);
}
#endif

#if defined(CONFIG_SY6970) && defined(CONFIG_ESP32S3_AUTO_SLEEP)

/****************************************************************************
 * Name: tdeckmax_usb_poll
 *
 * Description:
 *   Hold the chip out of light sleep while USB power is present.  Light
 *   sleep disconnects the USB Serial/JTAG port, which would take the USB
 *   console away from a computer.  There is no pin that tells the ESP32-S3
 *   about USB power, but the charger knows: its power good status.  On a
 *   wall charger this keeps the chip awake too, which costs nothing that
 *   matters while it charges.
 *
 *   Without USB power the port is disconnected as well.  Light sleep
 *   switches its pins off and on at every sleep, and a host would keep
 *   seeing a device appear for a moment, fail to enumerate it and, after
 *   a while, stop trying: it might then miss the device that stays when
 *   USB power comes back.  Connected once, with the chip held awake, the
 *   host sees a single clean attach.
 *
 ****************************************************************************/

static FAR struct battery_charger_dev_s *g_charger;
static struct work_s g_usb_work;
static bool g_usb_held;

static void tdeckmax_usb_poll(FAR void *arg)
{
  bool online;

  /* If the charger does not answer, stay awake rather than risk losing
   * the console.
   */

  if (g_charger->ops->online(g_charger, &online) < 0)
    {
      online = true;
    }

  if (online != g_usb_held)
    {
      if (online)
        {
          esp32s3_sleep_hold();
#ifdef CONFIG_ESP32S3_USBSERIAL
          esp32s3_usbserial_connect(true);
#endif
        }
      else
        {
#ifdef CONFIG_ESP32S3_USBSERIAL
          esp32s3_usbserial_connect(false);
#endif
          esp32s3_sleep_release();
        }

      g_usb_held = online;
    }

  work_queue(LPWORK, &g_usb_work, tdeckmax_usb_poll, NULL,
             MSEC2TICK(TDECKMAX_USB_POLL_MS));
}

#endif

/****************************************************************************
 * Name: tdeckmax_charger_initialize
 ****************************************************************************/

#ifdef CONFIG_SY6970
static int tdeckmax_charger_initialize(FAR struct i2c_master_s *i2c)
{
  FAR struct battery_charger_dev_s *charger;
  int ret;

  charger = sy6970_initialize(i2c, SY6970_I2C_ADDRESS,
                              TDECKMAX_BATTERY_I2C_FREQUENCY);
  if (charger == NULL)
    {
      return -ENODEV;
    }

  ret = charger->ops->voltage(charger,
                              CONFIG_LILYGO_TDECK_MAX_CHARGE_VOLTAGE);
  if (ret >= 0)
    {
      ret = charger->ops->current(charger,
                                  CONFIG_LILYGO_TDECK_MAX_CHARGE_CURRENT);
    }

  if (ret < 0)
    {
      /* Still register it: it charges with its previous settings, and its
       * status is worth having.
       */

      syslog(LOG_ERR, "ERROR: Failed to configure the charger: %d\n", ret);
    }

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  /* Check for USB power now, before the chip can first go to sleep, and
   * from then on every TDECKMAX_USB_POLL_MS.
   */

  g_charger = charger;
  tdeckmax_usb_poll(NULL);
#endif

  return battery_charger_register("/dev/charger0", charger);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_battery_initialize
 *
 * Description:
 *   Register the fuel gauge as /dev/batt0 and the charger as
 *   /dev/charger0.  A failure of one does not stop the other.
 *
 ****************************************************************************/

int tdeckmax_battery_initialize(void)
{
  FAR struct i2c_master_s *i2c;
  int result = OK;
  int ret;

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

#ifdef CONFIG_BQ27220
  ret = tdeckmax_gauge_initialize(i2c);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Fuel gauge: %d\n", ret);
      result = ret;
    }
#endif

#ifdef CONFIG_SY6970
  ret = tdeckmax_charger_initialize(i2c);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Charger: %d\n", ret);
      result = ret;
    }
#endif

  UNUSED(ret);
  return result;
}

#endif /* CONFIG_BQ27220 || CONFIG_SY6970 */
