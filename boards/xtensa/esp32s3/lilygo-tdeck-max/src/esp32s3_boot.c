/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_boot.c
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

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/board.h>
#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "lilygo-tdeck-max.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct parked_pin_s
{
  uint8_t pin;
  bool    level;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The e-paper, the microSD card and the SX1262 share one SPI bus, so every
 * chip select has to idle high before anything on the bus is powered or
 * accessed (the vendor firmware does the same).  The remaining lines are
 * parked in their inactive state so that they do not float during boot.
 */

static const struct parked_pin_s g_parked_pins[] =
{
  { BOARD_LORA_CS,           true  },
  { BOARD_SD_CS,             true  },
  { BOARD_EPD_CS,            true  },
  { BOARD_LORA_RST,          true  },   /* NRESET is active low */
  { BOARD_EPD_RST,           true  },   /* RST is active low */
  { BOARD_EPD_DC,            true  },   /* High selects data */
  { BOARD_EPD_FRONTLIGHT,    false },
  { BOARD_KEYBOARD_BACKLIGHT, false },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_board_initialize
 *
 * Description:
 *   All ESP32-S3 boards must provide the following entry point.
 *   This entry point is called early in the initialization -- after all
 *   memory has been configured and mapped but before any devices have been
 *   initialized.
 *
 ****************************************************************************/

void esp32s3_board_initialize(void)
{
  int i;

  for (i = 0; i < sizeof(g_parked_pins) / sizeof(g_parked_pins[0]); i++)
    {
      /* Latch the level first so that the pin never glitches low (which
       * would select the device) when the output driver is enabled.
       */

      esp_gpiowrite(g_parked_pins[i].pin, g_parked_pins[i].level);
      esp_configgpio(g_parked_pins[i].pin, OUTPUT);
    }
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize().  board_late_initialize() will
 *   be called immediately after up_initialize() is called and just before
 *   the initial application is started.  This additional initialization
 *   phase may be used, for example, to initialize board-specific device
 *   drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  /* Perform board-specific initialization */

  esp32s3_bringup();
}
#endif
