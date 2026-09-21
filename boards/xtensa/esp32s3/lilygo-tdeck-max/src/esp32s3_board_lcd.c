/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_board_lcd.c
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
 * The T-Deck Max carries a GoodDisplay GDEQ031T10, a 3.1 inch 240x320
 * monochrome e-paper panel driven by a UC8253 controller.  It sits on the
 * SPI2 bus it shares with the microSD slot and the SX1262 radio, so all
 * bus access goes through SPI_LOCK and the board's chip select helper.
 *
 * Besides the bus the panel needs three signals, all on native ESP32-S3
 * pins: D/C, which the board drives from its SPI_CMDDATA hook (see
 * esp32s3_board_spi.c), an active low reset, and an active low busy line
 * the controller holds down while it is refreshing.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <debug.h>

#include <nuttx/board.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/uc8253.h>
#include <nuttx/spi/spi.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "esp32s3_spi.h"

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_LCD_UC8253

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool tdeckmax_epd_set_rst(bool on);
static bool tdeckmax_epd_check_busy(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct lcd_dev_s *g_lcddev;

static const struct uc8253_priv_s g_uc8253_priv =
{
  .set_rst    = tdeckmax_epd_set_rst,
  .check_busy = tdeckmax_epd_check_busy,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_epd_set_rst
 *
 * Description:
 *   Drive the panel's reset line.  "on" is the logical reset state and the
 *   pin is active low, so asserting reset drives it low.
 *
 ****************************************************************************/

static bool tdeckmax_epd_set_rst(bool on)
{
  esp_gpiowrite(BOARD_EPD_RST, !on);
  return true;
}

/****************************************************************************
 * Name: tdeckmax_epd_check_busy
 *
 * Description:
 *   Return true while the controller is busy.  The panel holds BUSY low
 *   while it works and releases it high when it is ready.
 *
 ****************************************************************************/

static bool tdeckmax_epd_check_busy(void)
{
  return !esp_gpioread(BOARD_EPD_BUSY);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_lcd_initialize
 *
 * Description:
 *   Initialize the e-paper panel.  Called from up_fbinitialize() by way of
 *   fb_register(), and from lcddev_register().
 *
 ****************************************************************************/

int board_lcd_initialize(void)
{
  struct spi_dev_s *spi;

  if (g_lcddev != NULL)
    {
      return OK;
    }

  /* D/C and RST are already outputs, parked by the board's early boot code
   * so that the panel is not disturbed before its driver exists.  BUSY is
   * the one signal that has to be configured here.
   */

  esp_configgpio(BOARD_EPD_BUSY, INPUT | PULLUP);

  spi = esp32s3_spibus_initialize(ESP32S3_SPI2);
  if (spi == NULL)
    {
      lcderr("ERROR: Failed to initialize SPI port %d\n", ESP32S3_SPI2);
      return -ENODEV;
    }

  g_lcddev = uc8253_initialize(spi, &g_uc8253_priv);
  if (g_lcddev == NULL)
    {
      lcderr("ERROR: Failed to bind SPI port %d to the e-paper panel\n",
             ESP32S3_SPI2);
      return -ENODEV;
    }

  lcdinfo("E-paper panel bound to SPI port %d\n", ESP32S3_SPI2);
  return OK;
}

/****************************************************************************
 * Name: board_lcd_getdev
 *
 * Description:
 *   Return a reference to the LCD object for the specified display.
 *
 ****************************************************************************/

struct lcd_dev_s *board_lcd_getdev(int devno)
{
  return devno == 0 ? g_lcddev : NULL;
}

/****************************************************************************
 * Name: board_lcd_uninitialize
 *
 * Description:
 *   Put the panel to sleep.  The image stays on the glass: e-paper is
 *   bistable and holds its last frame with no power at all.
 *
 ****************************************************************************/

void board_lcd_uninitialize(void)
{
  if (g_lcddev != NULL)
    {
      g_lcddev->setpower(g_lcddev, 0);
    }
}

#endif /* CONFIG_LCD_UC8253 */
