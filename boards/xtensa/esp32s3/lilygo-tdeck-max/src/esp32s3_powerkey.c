/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_powerkey.c
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

/* The side button (S2 on the schematic, "BOOT") as a power key.
 *
 * It pulls GPIO0 low through a 10k pull-up; the other side button, S1,
 * is RST/EN and resets the chip, so this is the one a user interface can
 * have.  It is registered as a one-key keyboard, /dev/kbd2, that reports
 * KEYCODE_POWER as a special key (press and release), like the glass keys
 * on /dev/kbd1.
 *
 * The interrupt is taken on the low level, which light sleep can wake on;
 * the handler masks it and a worker debounces, reports the press, then
 * polls until the button is let go before unmasking again.  GPIO0 is a
 * strapping pin: held down through a reset it starts the ROM downloader,
 * as before.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/wqueue.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/keyboard.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_LILYGO_TDECK_MAX_POWERKEY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define POWERKEY_DEBOUNCE   MSEC2TICK(20)
#define POWERKEY_POLL       MSEC2TICK(30)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct keyboard_lowerhalf_s g_powerkey;
static struct work_s g_powerkey_work;
static bool g_powerkey_down;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void tdeckmax_powerkey_worker(FAR void *arg)
{
  bool down = !esp_gpioread(BOARD_BOOT_BUTTON);

  if (down != g_powerkey_down)
    {
      g_powerkey_down = down;
      keyboard_event(&g_powerkey, KEYCODE_POWER,
                     down ? KEYBOARD_SPECPRESS : KEYBOARD_SPECREL);
    }

  if (down)
    {
      /* Held: look again until it is let go (the level interrupt would
       * fire all the while)
       */

      work_queue(LPWORK, &g_powerkey_work, tdeckmax_powerkey_worker, NULL,
                 POWERKEY_POLL);
    }
  else
    {
      esp_gpioirqenable(BOARD_BOOT_BUTTON);
    }
}

static int tdeckmax_powerkey_interrupt(int irq, FAR void *context,
                                       FAR void *arg)
{
  esp_gpioirqdisable(BOARD_BOOT_BUTTON);
  work_queue(LPWORK, &g_powerkey_work, tdeckmax_powerkey_worker, NULL,
             POWERKEY_DEBOUNCE);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_powerkey_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_powerkey_initialize(void)
{
  int ret;

  ret = keyboard_register(&g_powerkey, "/dev/kbd2", 4);
  if (ret < 0)
    {
      ierr("ERROR: Failed to register /dev/kbd2: %d\n", ret);
      return ret;
    }

  esp_configgpio(BOARD_BOOT_BUTTON, INPUT | PULLUP | ONLOW);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  esp32s3_sleep_wake_on_gpio(BOARD_BOOT_BUTTON, false);
#endif

  ret = esp_gpio_irq(BOARD_BOOT_BUTTON, tdeckmax_powerkey_interrupt, NULL);
  if (ret < 0)
    {
      ierr("ERROR: Failed to attach GPIO%d: %d\n", BOARD_BOOT_BUTTON, ret);
      keyboard_unregister(&g_powerkey, "/dev/kbd2");
      return ret;
    }

  return OK;
}

#endif /* CONFIG_LILYGO_TDECK_MAX_POWERKEY */
