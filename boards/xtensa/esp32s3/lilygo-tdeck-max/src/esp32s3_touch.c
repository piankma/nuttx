/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_touch.c
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
 * The e-paper panel is covered by a CST3530 capacitive touch controller on
 * the shared I2C bus.  Its interrupt is GPIO12, pulled low for a report,
 * and its reset is the XL9555's touch_rst line.  The vendor's firmware maps
 * its coordinates straight onto the panel, with no swapping or mirroring.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/ioexpander/ioexpander.h>
#include <nuttx/input/cst3530.h>
#include <nuttx/input/kbd_codec.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "esp32s3_i2c.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_INPUT_CST3530

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  tdeckmax_touch_attach(FAR const struct cst3530_config_s *config,
                                  xcpt_t isr, FAR void *arg);
static void tdeckmax_touch_enable(FAR const struct cst3530_config_s *config,
                                  bool enable);
static void tdeckmax_touch_reset(FAR const struct cst3530_config_s *config,
                                 bool assert);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The controller's touch keys, by id */

static const uint32_t g_touch_keycodes[] =
{
  KEYCODE_F1, KEYCODE_F2, KEYCODE_F3
};

static const struct cst3530_config_s g_cst3530_config =
{
  .frequency = 400000,
  .flags     = 0,
  .keypath   = "/dev/kbd1",
  .keycodes  = g_touch_keycodes,
  .nkeys     = sizeof(g_touch_keycodes) / sizeof(g_touch_keycodes[0]),
  .attach    = tdeckmax_touch_attach,
  .enable    = tdeckmax_touch_enable,
  .reset     = tdeckmax_touch_reset,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_touch_attach
 ****************************************************************************/

static int tdeckmax_touch_attach(FAR const struct cst3530_config_s *config,
                                 xcpt_t isr, FAR void *arg)
{
  int ret;

  ret = esp_gpio_irq(BOARD_TOUCH_INT, isr, arg);
  if (ret < 0)
    {
      ierr("ERROR: Failed to attach GPIO%d: %d\n", BOARD_TOUCH_INT, ret);
      return ret;
    }

  /* esp_gpio_irq() enables the interrupt; the driver enables it itself
   * once the controller is awake.
   */

  esp_gpioirqdisable(BOARD_TOUCH_INT);
  return OK;
}

/****************************************************************************
 * Name: tdeckmax_touch_enable
 ****************************************************************************/

static void tdeckmax_touch_enable(FAR const struct cst3530_config_s *config,
                                  bool enable)
{
  if (enable)
    {
      esp_gpioirqenable(BOARD_TOUCH_INT);
    }
  else
    {
      esp_gpioirqdisable(BOARD_TOUCH_INT);
    }
}

/****************************************************************************
 * Name: tdeckmax_touch_reset
 ****************************************************************************/

static void tdeckmax_touch_reset(FAR const struct cst3530_config_s *config,
                                 bool assert)
{
  FAR struct ioexpander_dev_s *ioe = tdeckmax_xl9555_get();

  if (ioe != NULL)
    {
      IOEXP_WRITEPIN(ioe, XL9555_PIN_TOUCH_RST, !assert);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_touch_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_touch_initialize(void)
{
  FAR struct i2c_master_s *i2c;

  /* Level triggered, for the same reason as the keyboard's: light sleep
   * only wakes on levels, and edges that arrive while it sleeps are lost.
   * The driver masks the interrupt until it has read the report.
   */

  esp_configgpio(BOARD_TOUCH_INT, INPUT | PULLUP | ONLOW);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  esp32s3_sleep_wake_on_gpio(BOARD_TOUCH_INT, false);
#endif

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  return cst3530_register("/dev/input0", i2c, CST3530_I2C_ADDRESS,
                          &g_cst3530_config);
}

#endif /* CONFIG_INPUT_CST3530 */
