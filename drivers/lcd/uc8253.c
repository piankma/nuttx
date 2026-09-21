/****************************************************************************
 * drivers/lcd/uc8253.c
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
 * Driver for the UltraChip UC8253 e-paper controller, as used by the
 * GoodDisplay GDEQ031T10 3.1 inch 240x320 monochrome panel.
 *
 * An e-paper refresh takes about a second and wears the panel, so the
 * driver never refreshes on its own: putrun() and putarea() only update a
 * shadow framebuffer, and the panel is written and refreshed when redraw()
 * is called.  Through the LCD framebuffer shim that is an FBIO_UPDATE
 * ioctl, so an application draws as often as it likes and pays for one
 * refresh when it asks for one.
 *
 * The controller keeps two frame buffers, "previous" (DTM1) and "current"
 * (DTM2), and drives each pixel from the transition between them.  After a
 * refresh the current frame becomes the previous one, so steady state only
 * needs DTM2 to be written.
 *
 * The first refresh after a reset is the exception, and is treated as a
 * clear: both buffers are written white and the frame that triggered it is
 * dropped.  That is deliberate.  The previous buffer would otherwise hold
 * nothing meaningful, and the LCD framebuffer front end flushes its own
 * framebuffer the moment it registers, which has just been allocated and
 * zeroed: on this panel all zeroes is all black, so without this a board
 * would paint its screen black on every boot.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/sched.h>
#include <nuttx/spi/spi.h>
#include <nuttx/lcd/lcd.h>
#include <nuttx/lcd/uc8253.h>

#ifdef CONFIG_LCD_UC8253

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Geometry.  The panel is 240x320 and the driver presents it in its native
 * portrait orientation; a rotated presentation would have to transpose every
 * run, which is not implemented.
 */

#define UC8253_XRES          240
#define UC8253_YRES          320
#define UC8253_BPP           1
#define UC8253_COLORFMT      FB_FMT_Y1

#define UC8253_ROWSIZE       (UC8253_XRES / 8)              /* 30 bytes */
#define UC8253_FBSIZE        (UC8253_ROWSIZE * UC8253_YRES) /* 9600 bytes */

/* Controller commands (UC8253 datasheet) */

#define UC8253_PSR           0x00  /* Panel setting */
#define UC8253_POWER_OFF     0x02  /* Power off */
#define UC8253_POWER_ON      0x04  /* Power on */
#define UC8253_DEEP_SLEEP    0x07  /* Deep sleep */
#define UC8253_DTM1          0x10  /* Data start transmission 1 (previous) */
#define UC8253_DRF           0x12  /* Display refresh */
#define UC8253_DTM2          0x13  /* Data start transmission 2 (current) */
#define UC8253_CDI           0x50  /* VCOM and data interval setting */
#define UC8253_PTL           0x90  /* Partial window */
#define UC8253_PTIN          0x91  /* Partial in */
#define UC8253_PTOUT         0x92  /* Partial out */
#define UC8253_CCSET         0xe0  /* Cascade setting */
#define UC8253_TSSET         0xe5  /* Force temperature */

/* Panel setting: LUT from OTP, scan up, shift right, booster on, no reset.
 * The second byte is the controller's default VCOM/resolution selection.
 */

#define UC8253_PSR_RUN       0x1f
#define UC8253_PSR_SOFTRESET 0x1e
#define UC8253_PSR_BYTE2     0x0d

/* Data interval: the value differs between a full and a partial refresh */

#define UC8253_CDI_FULL      0x97
#define UC8253_CDI_PARTIAL   0xd7

/* Deep sleep check code, without which the command is ignored */

#define UC8253_DEEP_SLEEP_CHECK 0xa5

/* The controller picks its waveform from a measured temperature.  Forcing
 * the temperature selects a faster waveform: 90 shortens a full refresh
 * from about 3 s to about 1 s at the cost of the low temperature range.
 */

#define UC8253_TSSET_FAST    0x5a  /* 90 degrees, ~1.0 s full refresh */
#define UC8253_TSSET_PART    0x79  /* 121 degrees, ~0.7 s partial refresh */
#define UC8253_CCSET_FIX     0x02  /* Use the forced temperature */

/* Timeouts, in milliseconds.  These bound how long the driver waits for the
 * BUSY line and are deliberately generous: the panel is slower when cold,
 * and the alternative to waiting is a torn image.
 */

#define UC8253_BUSY_POLL_MS  5
#define UC8253_PWRON_TMO_MS  500
#define UC8253_PWROFF_TMO_MS 500
#define UC8253_REFRESH_TMO_MS 8000

/* Reset pulse width, in milliseconds.  The datasheet asks for at least 10 ms
 * either side of the pulse.
 */

#define UC8253_RESET_MS      10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct uc8253_dev_s
{
  /* Publicly visible device structure */

  struct lcd_dev_s dev;

  /* Private driver state */

  FAR struct spi_dev_s *spi;                  /* SPI bus the panel is on */
  FAR const struct uc8253_priv_s *board_priv; /* Board specific hooks */
  bool on;                                    /* Panel is powered up */
  bool configured;                            /* Init sequence has run */
  bool initial;                               /* No frame written yet */

  /* Region of the shadow framebuffer touched since the last refresh, as
   * inclusive pixel coordinates.  Tracking it lets redraw() refresh just
   * the part that changed, which on e-paper matters less for speed than
   * for not flashing the whole screen on every small update.  x1 > x2
   * means nothing has changed.
   */

  fb_coord_t x1;
  fb_coord_t y1;
  fb_coord_t x2;
  fb_coord_t y2;

  /* Partial refreshes since the last full one.  Partial refreshes leave a
   * little charge behind each time, so a full one is forced periodically
   * to clear the accumulated ghosting.
   */

  uint16_t partials;

  /* Shadow framebuffer.  The controller's RAM cannot be read back over this
   * interface, so the driver keeps the frame here and pushes it on redraw.
   */

  uint8_t shadow_fb[UC8253_FBSIZE];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* SPI helpers */

static void uc8253_configspi(FAR struct spi_dev_s *spi);
static void uc8253_lock(FAR struct uc8253_dev_s *priv);
static void uc8253_unlock(FAR struct uc8253_dev_s *priv);
static void uc8253_sendcmd(FAR struct uc8253_dev_s *priv, uint8_t cmd);
static void uc8253_senddata(FAR struct uc8253_dev_s *priv,
                            FAR const uint8_t *data, size_t len);
static void uc8253_sendfill(FAR struct uc8253_dev_s *priv, uint8_t value,
                            size_t len);
static void uc8253_sendcmd1(FAR struct uc8253_dev_s *priv, uint8_t cmd,
                            uint8_t arg);

/* Panel helpers */

static int  uc8253_busywait(FAR struct uc8253_dev_s *priv, int timeout_ms);
static void uc8253_reset(FAR struct uc8253_dev_s *priv);
static int  uc8253_configure(FAR struct uc8253_dev_s *priv);
static int  uc8253_poweron(FAR struct uc8253_dev_s *priv);
static int  uc8253_poweroff(FAR struct uc8253_dev_s *priv);
static int  uc8253_drive(FAR struct uc8253_dev_s *priv);
static int  uc8253_clear(FAR struct uc8253_dev_s *priv);
static void uc8253_dirty(FAR struct uc8253_dev_s *priv, fb_coord_t x1,
                         fb_coord_t y1, fb_coord_t x2, fb_coord_t y2);
static void uc8253_setwindow(FAR struct uc8253_dev_s *priv, fb_coord_t x,
                             fb_coord_t y, fb_coord_t w, fb_coord_t h);
static int  uc8253_refresh(FAR struct uc8253_dev_s *priv);

/* LCD data transfer methods */

static int  uc8253_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                          fb_coord_t col, FAR const uint8_t *buffer,
                          size_t npixels);
static int  uc8253_putarea(FAR struct lcd_dev_s *dev, fb_coord_t row_start,
                           fb_coord_t row_end, fb_coord_t col_start,
                           fb_coord_t col_end, FAR const uint8_t *buffer,
                           fb_coord_t stride);
static int  uc8253_getrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                          fb_coord_t col, FAR uint8_t *buffer,
                          size_t npixels);
static int  uc8253_redraw(FAR struct lcd_dev_s *dev);

/* LCD configuration */

static int  uc8253_getvideoinfo(FAR struct lcd_dev_s *dev,
                                FAR struct fb_videoinfo_s *vinfo);
static int  uc8253_getplaneinfo(FAR struct lcd_dev_s *dev, unsigned int pno,
                                FAR struct lcd_planeinfo_s *pinfo);

/* LCD specific controls */

static int  uc8253_getpower(FAR struct lcd_dev_s *dev);
static int  uc8253_setpower(FAR struct lcd_dev_s *dev, int power);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One raster line of working memory, as required by the LCD interface */

static uint8_t g_runbuffer[UC8253_ROWSIZE];

static const struct fb_videoinfo_s g_videoinfo =
{
  .fmt     = UC8253_COLORFMT,
  .xres    = UC8253_XRES,
  .yres    = UC8253_YRES,
  .nplanes = 1,
};

static const struct lcd_planeinfo_s g_planeinfo =
{
  .putrun  = uc8253_putrun,
  .putarea = uc8253_putarea,
  .getrun  = uc8253_getrun,
  .getarea = NULL,
  .redraw  = uc8253_redraw,
  .buffer  = (FAR uint8_t *)g_runbuffer,
  .bpp     = UC8253_BPP,
};

static const struct lcd_dev_s g_lcd_epaper_dev =
{
  .getvideoinfo = uc8253_getvideoinfo,
  .getplaneinfo = uc8253_getplaneinfo,
  .getpower     = uc8253_getpower,
  .setpower     = uc8253_setpower,
};

/* A single panel is supported */

static struct uc8253_dev_s g_epaperdev;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uc8253_bitcpy
 *
 * Description:
 *   Copy nbits from the start of src to dest, beginning dest_offset bits
 *   into the first destination byte.  Bits are packed most significant
 *   first, which is both the controller's layout and NuttX's packed 1bpp
 *   layout, so a byte aligned copy degenerates into memcpy.
 *
 ****************************************************************************/

static void uc8253_bitcpy(FAR uint8_t *dest, int dest_offset,
                          FAR const uint8_t *src, size_t nbits)
{
  uint8_t val;
  uint8_t mask;

  /* The aligned case is the common one: whole bytes, no shifting */

  if (dest_offset == 0)
    {
      memcpy(dest, src, nbits >> 3);
      dest += nbits >> 3;
      src  += nbits >> 3;
      nbits &= 7;

      if (nbits > 0)
        {
          mask   = (uint8_t)(0xff << (8 - nbits));
          *dest &= ~mask;
          *dest |= *src & mask;
        }

      return;
    }

  /* Straddling case: every source byte spans two destination bytes */

  while (nbits >= 8)
    {
      val = *src++;

      mask   = 0xff >> dest_offset;
      *dest &= ~mask;
      *dest |= val >> dest_offset;
      dest++;

      mask   = (uint8_t)(0xff << (8 - dest_offset));
      *dest &= ~mask;
      *dest |= (uint8_t)(val << (8 - dest_offset));

      nbits -= 8;
    }

  if (nbits > 0)
    {
      val = *src;

      if (nbits + dest_offset <= 8)
        {
          mask   = (uint8_t)((0xff << (8 - nbits)) >> dest_offset);
          *dest &= ~mask;
          *dest |= (val >> dest_offset) & mask;
        }
      else
        {
          mask   = 0xff >> dest_offset;
          *dest &= ~mask;
          *dest |= val >> dest_offset;
          dest++;

          nbits -= 8 - dest_offset;
          mask   = (uint8_t)(0xff << (8 - nbits));
          *dest &= ~mask;
          *dest |= (uint8_t)(val << (8 - dest_offset)) & mask;
        }
    }
}

/****************************************************************************
 * Name: uc8253_configspi
 ****************************************************************************/

static void uc8253_configspi(FAR struct spi_dev_s *spi)
{
  SPI_SETMODE(spi, CONFIG_LCD_UC8253_SPIMODE);
  SPI_SETBITS(spi, 8);
  SPI_HWFEATURES(spi, 0);
  SPI_SETFREQUENCY(spi, CONFIG_LCD_UC8253_FREQUENCY);
}

/****************************************************************************
 * Name: uc8253_lock / uc8253_unlock
 *
 * Description:
 *   Claim the (possibly shared) SPI bus for a whole panel transaction.  The
 *   chip select is pulsed per command inside the lock, which is what the
 *   panel vendor's own code does.
 *
 ****************************************************************************/

static void uc8253_lock(FAR struct uc8253_dev_s *priv)
{
  SPI_LOCK(priv->spi, true);
  uc8253_configspi(priv->spi);
}

static void uc8253_unlock(FAR struct uc8253_dev_s *priv)
{
  SPI_LOCK(priv->spi, false);
}

/****************************************************************************
 * Name: uc8253_sendcmd
 ****************************************************************************/

static void uc8253_sendcmd(FAR struct uc8253_dev_s *priv, uint8_t cmd)
{
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_SEND(priv->spi, cmd);
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
}

/****************************************************************************
 * Name: uc8253_senddata
 ****************************************************************************/

static void uc8253_senddata(FAR struct uc8253_dev_s *priv,
                            FAR const uint8_t *data, size_t len)
{
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);
  SPI_SNDBLOCK(priv->spi, data, len);
  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
}

/****************************************************************************
 * Name: uc8253_sendfill
 *
 * Description:
 *   Send the same byte len times, used to clear the controller's RAM
 *   without needing a buffer of the full frame size.
 *
 ****************************************************************************/

static void uc8253_sendfill(FAR struct uc8253_dev_s *priv, uint8_t value,
                            size_t len)
{
  size_t i;

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);

  for (i = 0; i < len; i++)
    {
      SPI_SEND(priv->spi, value);
    }

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
}

/****************************************************************************
 * Name: uc8253_sendcmd1
 *
 * Description:
 *   Send a command that takes a single parameter byte.
 *
 ****************************************************************************/

static void uc8253_sendcmd1(FAR struct uc8253_dev_s *priv, uint8_t cmd,
                            uint8_t arg)
{
  uc8253_sendcmd(priv, cmd);
  uc8253_senddata(priv, &arg, 1);
}

/****************************************************************************
 * Name: uc8253_busywait
 *
 * Description:
 *   Wait for the panel to drop its BUSY line.  Without a board hook there is
 *   nothing to poll, so fall back on sleeping for the whole timeout, which
 *   is correct but slow.
 *
 * Returned Value:
 *   OK if the panel became ready, -ETIMEDOUT if it never did.
 *
 ****************************************************************************/

static int uc8253_busywait(FAR struct uc8253_dev_s *priv, int timeout_ms)
{
  int waited;

  if (priv->board_priv == NULL || priv->board_priv->check_busy == NULL)
    {
      nxsched_usleep(timeout_ms * 1000);
      return OK;
    }

  /* The panel takes a moment to assert BUSY after a command */

  nxsched_usleep(1000);

  for (waited = 0; waited < timeout_ms; waited += UC8253_BUSY_POLL_MS)
    {
      if (!priv->board_priv->check_busy())
        {
          return OK;
        }

      nxsched_usleep(UC8253_BUSY_POLL_MS * 1000);
    }

  lcderr("ERROR: Panel stayed busy for more than %d ms\n", timeout_ms);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: uc8253_reset
 ****************************************************************************/

static void uc8253_reset(FAR struct uc8253_dev_s *priv)
{
  if (priv->board_priv == NULL || priv->board_priv->set_rst == NULL)
    {
      return;
    }

  priv->board_priv->set_rst(false);
  nxsched_usleep(UC8253_RESET_MS * 1000);
  priv->board_priv->set_rst(true);
  nxsched_usleep(UC8253_RESET_MS * 1000);
  priv->board_priv->set_rst(false);
  nxsched_usleep(UC8253_RESET_MS * 1000);
}

/****************************************************************************
 * Name: uc8253_configure
 *
 * Description:
 *   Reset the controller and put it in a known state.  The panel's RAM is
 *   undefined afterwards, so the next frame has to write both buffers.
 *
 ****************************************************************************/

static int uc8253_configure(FAR struct uc8253_dev_s *priv)
{
  static const uint8_t psr[] =
    {
      UC8253_PSR_RUN, UC8253_PSR_BYTE2
    };

  uc8253_reset(priv);

  uc8253_lock(priv);
  uc8253_sendcmd(priv, UC8253_PSR);
  uc8253_senddata(priv, psr, sizeof(psr));
  uc8253_unlock(priv);

  priv->configured = true;
  priv->initial    = true;

  lcdinfo("Panel configured\n");
  return OK;
}

/****************************************************************************
 * Name: uc8253_poweron
 *
 * Description:
 *   Turn on the panel driving voltages.  Must be held by the caller's lock.
 *
 ****************************************************************************/

static int uc8253_poweron(FAR struct uc8253_dev_s *priv)
{
  uc8253_sendcmd(priv, UC8253_POWER_ON);
  return uc8253_busywait(priv, UC8253_PWRON_TMO_MS);
}

/****************************************************************************
 * Name: uc8253_poweroff
 *
 * Description:
 *   Turn off the panel driving voltages.  The image stays on the screen and
 *   the controller's RAM is retained; leaving the voltages on instead makes
 *   the image fade.  Must be held by the caller's lock.
 *
 ****************************************************************************/

static int uc8253_poweroff(FAR struct uc8253_dev_s *priv)
{
  uc8253_sendcmd(priv, UC8253_POWER_OFF);
  return uc8253_busywait(priv, UC8253_PWROFF_TMO_MS);
}

/****************************************************************************
 * Name: uc8253_drive
 *
 * Description:
 *   Run one full refresh from whatever is already in the controller's two
 *   frame buffers.  Must be held by the caller's lock.
 *
 ****************************************************************************/

static int uc8253_drive(FAR struct uc8253_dev_s *priv)
{
  int ret;

#ifdef CONFIG_LCD_UC8253_FASTUPDATE
  uc8253_sendcmd1(priv, UC8253_CCSET, UC8253_CCSET_FIX);
  uc8253_sendcmd1(priv, UC8253_TSSET, UC8253_TSSET_FAST);
#endif

  uc8253_sendcmd1(priv, UC8253_CDI, UC8253_CDI_FULL);

  ret = uc8253_poweron(priv);
  if (ret < 0)
    {
      return ret;
    }

  uc8253_sendcmd(priv, UC8253_DRF);
  return uc8253_busywait(priv, UC8253_REFRESH_TMO_MS);
}

/****************************************************************************
 * Name: uc8253_dirty
 *
 * Description:
 *   Grow the dirty region to cover the given inclusive rectangle.
 *
 ****************************************************************************/

static void uc8253_dirty(FAR struct uc8253_dev_s *priv, fb_coord_t x1,
                         fb_coord_t y1, fb_coord_t x2, fb_coord_t y2)
{
  if (priv->x1 > priv->x2)
    {
      /* Nothing was dirty, so the new rectangle is the whole region */

      priv->x1 = x1;
      priv->y1 = y1;
      priv->x2 = x2;
      priv->y2 = y2;
      return;
    }

  if (x1 < priv->x1)
    {
      priv->x1 = x1;
    }

  if (y1 < priv->y1)
    {
      priv->y1 = y1;
    }

  if (x2 > priv->x2)
    {
      priv->x2 = x2;
    }

  if (y2 > priv->y2)
    {
      priv->y2 = y2;
    }
}

/****************************************************************************
 * Name: uc8253_cleandirty
 ****************************************************************************/

static void uc8253_cleandirty(FAR struct uc8253_dev_s *priv)
{
  priv->x1 = UC8253_XRES;
  priv->y1 = UC8253_YRES;
  priv->x2 = 0;
  priv->y2 = 0;
}

/****************************************************************************
 * Name: uc8253_setwindow
 *
 * Description:
 *   Tell the controller which part of its RAM the next transfer or refresh
 *   applies to.  The horizontal coordinates are byte addressed, so they are
 *   rounded outwards to a byte boundary; the vertical ones are exact and
 *   need two bytes each because the panel is 320 rows tall.
 *
 *   Must be held by the caller's lock.
 *
 ****************************************************************************/

static void uc8253_setwindow(FAR struct uc8253_dev_s *priv, fb_coord_t x,
                             fb_coord_t y, fb_coord_t w, fb_coord_t h)
{
  uint16_t xe = (x + w - 1) | 0x0007;
  uint16_t ye = y + h - 1;
  uint8_t args[7];

  x &= ~0x0007;

  args[0] = (uint8_t)x;
  args[1] = (uint8_t)xe;
  args[2] = (uint8_t)(y >> 8);
  args[3] = (uint8_t)(y & 0xff);
  args[4] = (uint8_t)(ye >> 8);
  args[5] = (uint8_t)(ye & 0xff);
  args[6] = 0x01;

  uc8253_sendcmd(priv, UC8253_PTL);
  uc8253_senddata(priv, args, sizeof(args));
}

/****************************************************************************
 * Name: uc8253_clear
 *
 * Description:
 *   Drive the whole panel to white.  Both controller buffers are written,
 *   so this also gives the "previous" buffer a defined value, which every
 *   later differential refresh depends on.
 *
 ****************************************************************************/

static int uc8253_clear(FAR struct uc8253_dev_s *priv)
{
  int ret;

  uc8253_lock(priv);

  uc8253_sendcmd(priv, UC8253_DTM1);
  uc8253_sendfill(priv, 0xff, UC8253_FBSIZE);
  uc8253_sendcmd(priv, UC8253_DTM2);
  uc8253_sendfill(priv, 0xff, UC8253_FBSIZE);

  ret = uc8253_drive(priv);

  uc8253_poweroff(priv);
  uc8253_unlock(priv);

  if (ret >= 0)
    {
      priv->initial = false;
    }

  return ret;
}

/****************************************************************************
 * Name: uc8253_refresh
 *
 * Description:
 *   Push the shadow framebuffer to the controller and run a full refresh.
 *
 *   The controller drives each pixel from the transition between its
 *   "previous" and "current" buffers, and after a refresh the current frame
 *   becomes the previous one.  In steady state that means only the current
 *   buffer has to be written.
 *
 ****************************************************************************/

static int uc8253_refresh(FAR struct uc8253_dev_s *priv)
{
  int ret;

  uc8253_lock(priv);

  uc8253_sendcmd(priv, UC8253_DTM2);
  uc8253_senddata(priv, priv->shadow_fb, UC8253_FBSIZE);

  ret = uc8253_drive(priv);

  uc8253_poweroff(priv);
  uc8253_unlock(priv);

  if (ret >= 0)
    {
      priv->partials = 0;
    }

  return ret;
}

#ifdef CONFIG_LCD_UC8253_PARTIAL

/****************************************************************************
 * Name: uc8253_drivepart
 *
 * Description:
 *   Run a partial refresh over the window already selected by the caller.
 *   The waveform is a different one from the full refresh, which is what
 *   keeps the rest of the screen from flashing.  Must be held by the
 *   caller's lock.
 *
 ****************************************************************************/

static int uc8253_drivepart(FAR struct uc8253_dev_s *priv)
{
  int ret;

  uc8253_sendcmd1(priv, UC8253_CCSET, UC8253_CCSET_FIX);
  uc8253_sendcmd1(priv, UC8253_TSSET, UC8253_TSSET_PART);
  uc8253_sendcmd1(priv, UC8253_CDI, UC8253_CDI_PARTIAL);

  ret = uc8253_poweron(priv);
  if (ret < 0)
    {
      return ret;
    }

  uc8253_sendcmd(priv, UC8253_DRF);
  return uc8253_busywait(priv, UC8253_REFRESH_TMO_MS);
}

/****************************************************************************
 * Name: uc8253_refreshpart
 *
 * Description:
 *   Push the dirty rectangle of the shadow framebuffer to the controller
 *   and refresh only that part of the panel.  The rectangle is widened to
 *   byte boundaries first because the controller addresses its RAM by the
 *   byte horizontally.
 *
 ****************************************************************************/

static int uc8253_refreshpart(FAR struct uc8253_dev_s *priv)
{
  fb_coord_t x = priv->x1 & ~0x0007;
  fb_coord_t y = priv->y1;
  fb_coord_t w = ((priv->x2 | 0x0007) - x) + 1;
  fb_coord_t h = (priv->y2 - y) + 1;
  fb_coord_t row;
  int ret;

  uc8253_lock(priv);

  /* Write the rectangle into the controller's current frame buffer */

  uc8253_sendcmd(priv, UC8253_PTIN);
  uc8253_setwindow(priv, x, y, w, h);
  uc8253_sendcmd(priv, UC8253_DTM2);

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);

  for (row = y; row < y + h; row++)
    {
      SPI_SNDBLOCK(priv->spi,
                   priv->shadow_fb + row * UC8253_ROWSIZE + (x >> 3),
                   w >> 3);
    }

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
  uc8253_sendcmd(priv, UC8253_PTOUT);

  /* Then refresh just that window */

  uc8253_sendcmd(priv, UC8253_PTIN);
  uc8253_setwindow(priv, x, y, w, h);

  ret = uc8253_drivepart(priv);

  uc8253_sendcmd(priv, UC8253_PTOUT);
  uc8253_poweroff(priv);
  uc8253_unlock(priv);

  if (ret >= 0)
    {
      priv->partials++;
    }

  return ret;
}

#endif /* CONFIG_LCD_UC8253_PARTIAL */

/****************************************************************************
 * Name: uc8253_putrun
 *
 * Description:
 *   Write a partial raster line to the shadow framebuffer.  Nothing reaches
 *   the panel until redraw().
 *
 ****************************************************************************/

static int uc8253_putrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, FAR const uint8_t *buffer,
                         size_t npixels)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;

  if (row >= UC8253_YRES || col >= UC8253_XRES)
    {
      return -EINVAL;
    }

  if (npixels > (size_t)(UC8253_XRES - col))
    {
      npixels = UC8253_XRES - col;
    }

  uc8253_bitcpy(priv->shadow_fb + row * UC8253_ROWSIZE + (col >> 3),
                col & 7, buffer, npixels);

  uc8253_dirty(priv, col, row, col + npixels - 1, row);

  return OK;
}

/****************************************************************************
 * Name: uc8253_putarea
 *
 * Description:
 *   Write a rectangular area to the shadow framebuffer.  The framebuffer
 *   shim byte aligns col_start for a 1bpp panel, so the common full width
 *   case is one memcpy per row.
 *
 ****************************************************************************/

static int uc8253_putarea(FAR struct lcd_dev_s *dev, fb_coord_t row_start,
                          fb_coord_t row_end, fb_coord_t col_start,
                          fb_coord_t col_end, FAR const uint8_t *buffer,
                          fb_coord_t stride)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;
  size_t npixels;
  fb_coord_t row;

  if (row_end >= UC8253_YRES)
    {
      row_end = UC8253_YRES - 1;
    }

  if (col_end >= UC8253_XRES)
    {
      col_end = UC8253_XRES - 1;
    }

  if (row_start > row_end || col_start > col_end)
    {
      return -EINVAL;
    }

  npixels = col_end - col_start + 1;

  for (row = row_start; row <= row_end; row++)
    {
      FAR uint8_t *dst = priv->shadow_fb + row * UC8253_ROWSIZE +
                         (col_start >> 3);

      uc8253_bitcpy(dst, col_start & 7,
                    buffer + (row - row_start) * stride, npixels);
    }

  uc8253_dirty(priv, col_start, row_start, col_end, row_end);

  return OK;
}

/****************************************************************************
 * Name: uc8253_getrun
 *
 * Description:
 *   Read a partial raster line back from the shadow framebuffer.  The
 *   controller's RAM cannot be read over this interface, so the shadow copy
 *   is the only source.
 *
 ****************************************************************************/

static int uc8253_getrun(FAR struct lcd_dev_s *dev, fb_coord_t row,
                         fb_coord_t col, FAR uint8_t *buffer,
                         size_t npixels)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;
  FAR const uint8_t *src;
  size_t i;

  if (row >= UC8253_YRES || col >= UC8253_XRES)
    {
      return -EINVAL;
    }

  if (npixels > (size_t)(UC8253_XRES - col))
    {
      npixels = UC8253_XRES - col;
    }

  src = priv->shadow_fb + row * UC8253_ROWSIZE;

  /* Assemble the run most significant bit first, starting at "col" */

  for (i = 0; i < npixels; i++)
    {
      size_t bit = col + i;
      uint8_t pixel = (src[bit >> 3] >> (7 - (bit & 7))) & 1;

      if ((i & 7) == 0)
        {
          buffer[i >> 3] = 0;
        }

      buffer[i >> 3] |= (uint8_t)(pixel << (7 - (i & 7)));
    }

  return OK;
}

/****************************************************************************
 * Name: uc8253_redraw
 ****************************************************************************/

static int uc8253_redraw(FAR struct lcd_dev_s *dev)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;
  int ret;

  if (!priv->on)
    {
      lcdwarn("WARNING: Refusing to redraw a powered down panel\n");
      return -EPERM;
    }

  if (!priv->configured)
    {
      ret = uc8253_configure(priv);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (priv->initial)
    {
      /* The LCD framebuffer front end flushes its framebuffer as soon as it
       * registers, and that buffer has just been allocated and zeroed, which
       * on this panel means every pixel black.  Nobody asked for a black
       * screen, so the first refresh after a reset is taken as a clear: the
       * panel is driven to white and that frame is dropped.  Doing it this
       * way also leaves the controller's "previous" buffer defined, which
       * every later differential refresh needs.
       */

      ret = uc8253_clear(priv);
      uc8253_cleandirty(priv);
      return ret;
    }

  if (priv->x1 > priv->x2)
    {
      /* Nothing has been drawn since the last refresh.  Refreshing anyway
       * would cost a second and wear the panel for no reason.
       */

      return OK;
    }

#ifdef CONFIG_LCD_UC8253_PARTIAL
  /* A partial refresh leaves the rest of the screen alone, so it is worth
   * using whenever the change does not cover the whole panel.  It does
   * leave a little ghosting behind, so a full refresh is forced every so
   * often to clean it up.
   */

  if ((priv->x1 > 0 || priv->y1 > 0 ||
       priv->x2 < UC8253_XRES - 1 || priv->y2 < UC8253_YRES - 1) &&
      (CONFIG_LCD_UC8253_FULL_EVERY == 0 ||
       priv->partials < CONFIG_LCD_UC8253_FULL_EVERY))
    {
      ret = uc8253_refreshpart(priv);
      uc8253_cleandirty(priv);
      return ret;
    }
#endif

  ret = uc8253_refresh(priv);
  uc8253_cleandirty(priv);
  return ret;
}

/****************************************************************************
 * Name: uc8253_getvideoinfo
 ****************************************************************************/

static int uc8253_getvideoinfo(FAR struct lcd_dev_s *dev,
                               FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(dev != NULL && vinfo != NULL);

  *vinfo = g_videoinfo;
  return OK;
}

/****************************************************************************
 * Name: uc8253_getplaneinfo
 ****************************************************************************/

static int uc8253_getplaneinfo(FAR struct lcd_dev_s *dev, unsigned int pno,
                               FAR struct lcd_planeinfo_s *pinfo)
{
  DEBUGASSERT(dev != NULL && pinfo != NULL && pno == 0);

  *pinfo = g_planeinfo;
  pinfo->dev = dev;
  return OK;
}

/****************************************************************************
 * Name: uc8253_getpower
 ****************************************************************************/

static int uc8253_getpower(FAR struct lcd_dev_s *dev)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;

  return priv->on ? CONFIG_LCD_MAXPOWER : 0;
}

/****************************************************************************
 * Name: uc8253_setpower
 *
 * Description:
 *   Power the panel up or down.  Powering down puts the controller in deep
 *   sleep, which loses its RAM; the image stays on the screen because
 *   e-paper is bistable.  The next redraw reconfigures the controller.
 *
 ****************************************************************************/

static int uc8253_setpower(FAR struct lcd_dev_s *dev, int power)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;

  lcdinfo("power: %d -> %d\n", priv->on ? CONFIG_LCD_MAXPOWER : 0, power);

  if (power > 0)
    {
      if (!priv->configured)
        {
          int ret = uc8253_configure(priv);

          if (ret < 0)
            {
              return ret;
            }
        }

      priv->on = true;
    }
  else if (priv->on)
    {
      /* Deep sleep needs the check code, and only takes effect once the
       * driving voltages are off.
       */

      uc8253_lock(priv);
      uc8253_poweroff(priv);
      uc8253_sendcmd1(priv, UC8253_DEEP_SLEEP, UC8253_DEEP_SLEEP_CHECK);
      uc8253_unlock(priv);

      priv->on         = false;
      priv->configured = false;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: uc8253_initialize
 *
 * Description:
 *   See include/nuttx/lcd/uc8253.h
 *
 ****************************************************************************/

FAR struct lcd_dev_s *
uc8253_initialize(FAR struct spi_dev_s *spi,
                  FAR const struct uc8253_priv_s *board_priv)
{
  FAR struct uc8253_dev_s *priv = &g_epaperdev;
  int ret;

  DEBUGASSERT(spi != NULL);

  priv->dev        = g_lcd_epaper_dev;
  priv->spi        = spi;
  priv->board_priv = board_priv;
  priv->on         = false;
  priv->configured = false;
  priv->initial    = true;
  priv->partials   = 0;

  uc8253_cleandirty(priv);

  /* Start from a white screen.  A set bit is white on this panel. */

  memset(priv->shadow_fb, 0xff, UC8253_FBSIZE);

  /* Reset and configure the controller, but leave the glass alone: a
   * refresh takes about a second and the caller decides when to pay for
   * one.  The first redraw starts the panel from a known white image.
   */

  ret = uc8253_setpower(&priv->dev, CONFIG_LCD_MAXPOWER);
  if (ret < 0)
    {
      lcderr("ERROR: Failed to power up the panel: %d\n", ret);
      return NULL;
    }

  lcdinfo("UC8253 ready: %dx%d, %d bpp\n",
          UC8253_XRES, UC8253_YRES, UC8253_BPP);

  return &priv->dev;
}

#endif /* CONFIG_LCD_UC8253 */
