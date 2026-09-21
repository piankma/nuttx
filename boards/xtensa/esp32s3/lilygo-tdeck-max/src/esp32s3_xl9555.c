/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_xl9555.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/ioexpander/ioexpander.h>
#include <nuttx/ioexpander/gpio.h>
#include <nuttx/ioexpander/pca9555.h>

#include <arch/board/board.h>

#include "esp32s3_i2c.h"
#include "lilygo-tdeck-max.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TDECKMAX_I2C_PORT 0

#ifdef CONFIG_LILYGO_TDECK_MAX_BOOT_LORA_POWER
#  define LORA_BOOT_LEVEL true
#else
#  define LORA_BOOT_LEVEL false
#endif

#ifdef CONFIG_LILYGO_TDECK_MAX_BOOT_GPS_POWER
#  define GPS_BOOT_LEVEL true
#else
#  define GPS_BOOT_LEVEL false
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct xl9555_pin_s
{
  uint8_t pin;                /* XL9555 pin number (0-7 = P0x, 8-15 = P1x) */
  FAR char *name;             /* Registered as /dev/<name> */
  bool initial;               /* Level driven at boot */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Default state of every line the XL9555 drives.  The XL9555 keeps its
 * state across an ESP32-S3 reset, so each line is always driven explicitly.
 *
 * Radios (modem, LoRa, GPS) and the audio amplifier are left off; the I2C
 * peripherals (IMU rail, haptic driver) are powered and the touch and
 * keyboard controllers are released from reset so that they show up on the
 * I2C bus.  Every line is available as /dev/<name> for runtime control.
 */

static struct xl9555_pin_s g_xl9555_pins[] =
{
  { XL9555_PIN_MODEM_PWR,    "modem_pwr",    false },
  { XL9555_PIN_LORA_EN,      "lora_en",      LORA_BOOT_LEVEL },
  { XL9555_PIN_GPS_EN,       "gps_en",       GPS_BOOT_LEVEL },
  { XL9555_PIN_IMU_1V8_EN,   "imu_en",       true },
  { XL9555_PIN_LORA_ANT,     "lora_ant",     true },   /* Internal antenna */
  { XL9555_PIN_MOTOR_EN,     "motor_en",     true },
  { XL9555_PIN_AMP_EN,       "amp_en",       false },
  { XL9555_PIN_TOUCH_RST,    "touch_rst",    true },   /* Active low */
  { XL9555_PIN_MODEM_PWRKEY, "modem_pwrkey", false },
  { XL9555_PIN_KEY_RST,      "key_rst",      true },   /* Active low */
  { XL9555_PIN_AUDIO_SEL,    "audio_sel",    false },  /* ES8311 */
};

static struct pca9555_config_s g_xl9555_config =
{
  .address   = BOARD_I2C_ADDR_XL9555,
  .frequency = CONFIG_LILYGO_TDECK_MAX_XL9555_FREQUENCY,
};

static struct ioexpander_dev_s *g_xl9555;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_xl9555_get
 ****************************************************************************/

struct ioexpander_dev_s *tdeckmax_xl9555_get(void)
{
  return g_xl9555;
}

/****************************************************************************
 * Name: tdeckmax_xl9555_initialize
 ****************************************************************************/

int tdeckmax_xl9555_initialize(void)
{
  struct i2c_master_s *i2c;
  int ret;
  int i;

  if (g_xl9555 != NULL)
    {
      return OK;
    }

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: XL9555: failed to get I2C%d\n",
             TDECKMAX_I2C_PORT);
      return -ENODEV;
    }

  g_xl9555 = pca9555_initialize(i2c, &g_xl9555_config);
  if (g_xl9555 == NULL)
    {
      syslog(LOG_ERR, "ERROR: XL9555: pca9555_initialize() failed\n");
      esp32s3_i2cbus_uninitialize(i2c);
      return -ENODEV;
    }

  for (i = 0; i < sizeof(g_xl9555_pins) / sizeof(g_xl9555_pins[0]); i++)
    {
      const struct xl9555_pin_s *p = &g_xl9555_pins[i];

      /* Load the output latch before enabling the output driver so that
       * the line goes straight to its intended level without a glitch.
       * The first access doubles as a presence check for the expander.
       */

      ret = IOEXP_WRITEPIN(g_xl9555, p->pin, p->initial);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: XL9555 not responding at 0x%02x: %d\n",
                 BOARD_I2C_ADDR_XL9555, ret);
          g_xl9555 = NULL;
          return ret;
        }

      ret = IOEXP_SETDIRECTION(g_xl9555, p->pin, IOEXPANDER_DIRECTION_OUT);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: XL9555: pin %d direction failed: %d\n",
                 p->pin, ret);
          return ret;
        }

      ret = gpio_lower_half_byname(g_xl9555, p->pin, GPIO_OUTPUT_PIN,
                                   p->name);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: XL9555: failed to register /dev/%s: %d\n",
                 p->name, ret);
          return ret;
        }
    }

  /* Give the touch controller a deterministic reset pulse so that it
   * leaves any half-powered state (same as the vendor firmware).
   */

  IOEXP_WRITEPIN(g_xl9555, XL9555_PIN_TOUCH_RST, false);
  up_mdelay(20);
  IOEXP_WRITEPIN(g_xl9555, XL9555_PIN_TOUCH_RST, true);
  up_mdelay(60);

  syslog(LOG_INFO, "XL9555 I/O expander ready at 0x%02x\n",
         BOARD_I2C_ADDR_XL9555);
  return OK;
}
