/****************************************************************************
 * include/nuttx/lcd/uc8253.h
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

#ifndef __INCLUDE_NUTTX_LCD_UC8253_H
#define __INCLUDE_NUTTX_LCD_UC8253_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#include <nuttx/fs/ioctl.h>

#ifdef CONFIG_LCD_UC8253

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* UC8253 configuration settings:
 *
 * CONFIG_LCD_UC8253_SPIMODE   - SPI mode (the controller samples on the
 *                               rising edge of SCL, so mode 0)
 * CONFIG_LCD_UC8253_FREQUENCY - SPI bus frequency
 * CONFIG_LCD_UC8253_FASTUPDATE - Override the temperature the controller
 *                               picks its waveform for, which shortens a
 *                               full refresh from about 3 s to about 1 s.
 * CONFIG_LCD_UC8253_ASYNC     - Refresh from a dedicated thread, so that
 *                               redraw() returns at once and a burst of
 *                               small updates costs one refresh.
 *
 * Required LCD driver settings:
 *
 * CONFIG_LCD_MAXPOWER must be 1 (the panel is either driven or not).
 *
 * Required SPI driver settings:
 *
 * CONFIG_SPI_CMDDATA - The controller distinguishes a command from its
 *   parameters with a separate D/C line, which the board drives from its
 *   SPI_CMDDATA hook.
 */

#ifndef CONFIG_SPI_CMDDATA
#  error "CONFIG_SPI_CMDDATA must be defined in your NuttX configuration"
#endif

/* Check power setting */

#if !defined(CONFIG_LCD_MAXPOWER)
#  define CONFIG_LCD_MAXPOWER 1
#endif

#if CONFIG_LCD_MAXPOWER != 1
#  warning "CONFIG_LCD_MAXPOWER exceeds supported maximum"
#  undef CONFIG_LCD_MAXPOWER
#  define CONFIG_LCD_MAXPOWER 1
#endif

/* The panel is 1bpp monochrome.  A set bit is white, a clear bit is black,
 * and the most significant bit of a byte is the leftmost pixel, which is
 * both the controller's native format and NuttX's packed 1bpp format.
 */

#ifdef CONFIG_NX_DISABLE_1BPP
#  warning "1 bit-per-pixel support needed"
#endif

#define UC8253_Y1_BLACK 0
#define UC8253_Y1_WHITE 1

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* ioctl commands, through the framebuffer (/dev/fbN) as well:
 *
 * UC8253IOC_FULLREFRESH - Make the next refresh a full one, whatever it
 *   covers, to clear the ghosting partial refreshes leave behind.
 *   Argument: none.
 * UC8253IOC_SETFULLEVERY - Force a full refresh after this many partial
 *   ones (0: never; CONFIG_LCD_UC8253_FULL_EVERY at start).
 *   Argument: the count, 0..65535.
 */

#define UC8253IOC_FULLREFRESH   _LCDIOC(0x80)
#define UC8253IOC_SETFULLEVERY  _LCDIOC(0x81)

/* Board specific hooks.  The controller needs two signals that are not part
 * of the SPI bus: an active low reset, and a busy line the controller holds
 * low while it is working.  Both are optional; without them the driver falls
 * back on fixed delays, which works but is slower and cannot report a panel
 * that never answers.
 */

struct uc8253_priv_s
{
  /* Drive the panel's reset line.  "on" is the logical reset state, so
   * true means the (active low) RST pin is driven low.  Return true if the
   * request was honoured.
   */

  bool (*set_rst)(bool on);

  /* Return true while the panel is busy, that is, while the (active low)
   * BUSY pin reads low.
   */

  bool (*check_busy)(void);
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

struct lcd_dev_s;  /* See include/nuttx/lcd/lcd.h */
struct spi_dev_s;  /* See include/nuttx/spi/spi.h */

/****************************************************************************
 * Name: uc8253_initialize
 *
 * Description:
 *   Initialize the UC8253 e-paper controller.  The panel is reset and
 *   configured, and its shadow framebuffer is cleared to white, but nothing
 *   is shown until the first redraw: an e-paper refresh takes about a second
 *   and holding it here would stall the boot.
 *
 * Input Parameters:
 *   spi        - A reference to the SPI driver instance the panel is on.
 *   board_priv - Board specific hooks, see struct uc8253_priv_s.  May be
 *                NULL, in which case reset and busy handling are skipped.
 *
 * Returned Value:
 *   On success, a reference to the LCD object for the panel.  NULL on any
 *   failure.
 *
 ****************************************************************************/

FAR struct lcd_dev_s *
uc8253_initialize(FAR struct spi_dev_s *spi,
                  FAR const struct uc8253_priv_s *board_priv);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_LCD_UC8253 */
#endif /* __INCLUDE_NUTTX_LCD_UC8253_H */
