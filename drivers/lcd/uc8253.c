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
 * With CONFIG_LCD_UC8253_ASYNC the refresh itself moves to a thread of its
 * own, and redraw() only wakes it.  That matters for callers that draw a
 * little at a time and ask for an update after each piece, as NX does:
 * the updates that arrive while a refresh runs all go out in the next one.
 *
 * The controller keeps two frame buffers, "previous" (DTM1) and "current"
 * (DTM2), and a partial refresh drives each pixel from the transition
 * between them: a pixel whose two values agree is left alone.  The
 * controller does not carry the current frame over into the previous one
 * after a refresh, so every partial refresh writes both, the previous one
 * from a copy of what the glass shows.  The vendor's code also soft resets
 * the controller after every refresh, with the note "needed, reason
 * unknown"; this driver does the same.
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
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/semaphore.h>
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
#define UC8253_CCSET_SENSOR  0x00  /* Use the measured temperature */

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
  bool initial;                               /* Controller RAM undefined */
  bool known;                                 /* glass_fb matches glass */
  bool dropped;                               /* First flush discarded */
  bool stale;                                 /* Last refresh failed */

  /* priv->panel serialises everything that talks to the controller and is
   * held for a whole refresh.  priv->fblock only protects the shadow
   * framebuffer and the dirty region, and is only ever held briefly, so
   * that drawing never has to wait for the panel.  When both are needed,
   * panel is taken first.
   */

  mutex_t panel;
  mutex_t fblock;

#ifdef CONFIG_LCD_UC8253_ASYNC
  sem_t kick;                                 /* Wakes the refresh thread */
#endif

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

  /* What the glass shows: the part of the shadow framebuffer that has been
   * refreshed.  A partial refresh needs it as the "previous" frame.  Only
   * the refresh path touches it, under priv->panel.
   */

  uint8_t glass_fb[UC8253_FBSIZE];
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
static void uc8253_softreset(FAR struct uc8253_dev_s *priv);
static int  uc8253_start(FAR struct uc8253_dev_s *priv, bool partial);
static int  uc8253_finish(FAR struct uc8253_dev_s *priv, bool partial);
static int  uc8253_clear(FAR struct uc8253_dev_s *priv);
static int  uc8253_reseed(FAR struct uc8253_dev_s *priv);
static int  uc8253_update(FAR struct uc8253_dev_s *priv);
#ifdef CONFIG_LCD_UC8253_ASYNC
static void uc8253_kick(FAR struct uc8253_dev_s *priv);
static int  uc8253_thread(int argc, FAR char *argv[]);
#endif
static void uc8253_dirty(FAR struct uc8253_dev_s *priv, fb_coord_t x1,
                         fb_coord_t y1, fb_coord_t x2, fb_coord_t y2);
static void uc8253_setwindow(FAR struct uc8253_dev_s *priv, fb_coord_t x,
                             fb_coord_t y, fb_coord_t w, fb_coord_t h);
static void uc8253_sendwindow(FAR struct uc8253_dev_s *priv, uint8_t cmd,
                              fb_coord_t x, fb_coord_t y, fb_coord_t w,
                              fb_coord_t h);
static bool uc8253_trim(FAR struct uc8253_dev_s *priv, FAR fb_coord_t *x,
                        FAR fb_coord_t *y, FAR fb_coord_t *w,
                        FAR fb_coord_t *h);

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
          /* Truncate to a byte before shifting right: 0xff << n is an int,
           * and shifting its high bits back down would widen the mask over
           * the pixels in front of the run.
           */

          mask   = (uint8_t)(0xff << (8 - nbits));
          mask >>= dest_offset;
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
 * Name: uc8253_softreset
 *
 * Description:
 *   Soft reset the controller and put the panel setting back.  The vendor's
 *   code does this after every refresh, noting only that it is needed.  The
 *   RAM is kept.  Must be held by the caller's lock.
 *
 ****************************************************************************/

static void uc8253_softreset(FAR struct uc8253_dev_s *priv)
{
  static const uint8_t reset[] =
    {
      UC8253_PSR_SOFTRESET, UC8253_PSR_BYTE2
    };

  static const uint8_t run[] =
    {
      UC8253_PSR_RUN, UC8253_PSR_BYTE2
    };

  uc8253_sendcmd(priv, UC8253_PSR);
  uc8253_senddata(priv, reset, sizeof(reset));
  up_mdelay(1);
  uc8253_sendcmd(priv, UC8253_PSR);
  uc8253_senddata(priv, run, sizeof(run));
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
 * Name: uc8253_sendwindow
 *
 * Description:
 *   Write a byte aligned rectangle of glass_fb into one of the controller's
 *   frame buffers.  Must be held by the caller's lock.
 *
 ****************************************************************************/

static void uc8253_sendwindow(FAR struct uc8253_dev_s *priv, uint8_t cmd,
                              fb_coord_t x, fb_coord_t y, fb_coord_t w,
                              fb_coord_t h)
{
  fb_coord_t row;

  uc8253_sendcmd(priv, UC8253_PTIN);
  uc8253_setwindow(priv, x, y, w, h);
  uc8253_sendcmd(priv, cmd);

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), true);
  SPI_CMDDATA(priv->spi, SPIDEV_DISPLAY(0), false);

  for (row = y; row < y + h; row++)
    {
      SPI_SNDBLOCK(priv->spi,
                   priv->glass_fb + row * UC8253_ROWSIZE + (x >> 3),
                   w >> 3);
    }

  SPI_SELECT(priv->spi, SPIDEV_DISPLAY(0), false);
  uc8253_sendcmd(priv, UC8253_PTOUT);
}

/****************************************************************************
 * Name: uc8253_trim
 *
 * Description:
 *   Shrink a byte aligned rectangle to the part where the shadow framebuffer
 *   differs from the glass.  Drawing often repaints what is already there:
 *   NX, for one, fills whole windows with the colour they already have, and
 *   refreshing that would flash the panel for nothing.  Called with
 *   priv->fblock held.
 *
 * Returned Value:
 *   false if nothing differs, and there is nothing to refresh.
 *
 ****************************************************************************/

static bool uc8253_trim(FAR struct uc8253_dev_s *priv, FAR fb_coord_t *x,
                        FAR fb_coord_t *y, FAR fb_coord_t *w,
                        FAR fb_coord_t *h)
{
  fb_coord_t bfirst = *x >> 3;
  fb_coord_t blast  = (*x + *w) >> 3;
  fb_coord_t b0     = blast;
  fb_coord_t b1     = 0;
  fb_coord_t y0     = *y + *h;
  fb_coord_t y1     = 0;
  fb_coord_t row;
  fb_coord_t b;
  size_t offset;

  for (row = *y; row < *y + *h; row++)
    {
      offset = row * UC8253_ROWSIZE;
      for (b = bfirst; b < blast; b++)
        {
          if (priv->shadow_fb[offset + b] != priv->glass_fb[offset + b])
            {
              b0 = b < b0 ? b : b0;
              b1 = b > b1 ? b : b1;
              y0 = row < y0 ? row : y0;
              y1 = row;
            }
        }
    }

  if (b0 > b1)
    {
      return false;
    }

  *x = b0 << 3;
  *w = (b1 - b0 + 1) << 3;
  *y = y0;
  *h = y1 - y0 + 1;
  return true;
}

/****************************************************************************
 * Name: uc8253_start
 *
 * Description:
 *   Start a refresh of whatever is in the controller's frame buffers.  For
 *   a partial refresh the caller has already selected the window.  Must be
 *   called with the SPI bus held.
 *
 ****************************************************************************/

static int uc8253_start(FAR struct uc8253_dev_s *priv, bool partial)
{
  int ret;

#ifdef CONFIG_LCD_UC8253_PARTIAL
  if (partial)
    {
      /* The partial waveform is a different one, which is what keeps the
       * rest of the screen from flashing.
       */

      uc8253_sendcmd1(priv, UC8253_CCSET, UC8253_CCSET_FIX);
      uc8253_sendcmd1(priv, UC8253_TSSET, UC8253_TSSET_PART);
      uc8253_sendcmd1(priv, UC8253_CDI, UC8253_CDI_PARTIAL);
    }
  else
#endif
    {
#ifdef CONFIG_LCD_UC8253_FASTUPDATE
      uc8253_sendcmd1(priv, UC8253_CCSET, UC8253_CCSET_FIX);
      uc8253_sendcmd1(priv, UC8253_TSSET, UC8253_TSSET_FAST);
#else
      /* A partial refresh forces the temperature; hand it back to the
       * sensor, or the full waveform would be chosen for the wrong one.
       */

      uc8253_sendcmd1(priv, UC8253_CCSET, UC8253_CCSET_SENSOR);
#endif
      uc8253_sendcmd1(priv, UC8253_CDI, UC8253_CDI_FULL);
    }

  ret = uc8253_poweron(priv);
  if (ret < 0)
    {
      return ret;
    }

  uc8253_sendcmd(priv, UC8253_DRF);
  return OK;
}

/****************************************************************************
 * Name: uc8253_finish
 *
 * Description:
 *   Wait for a refresh started by uc8253_start() to complete, then turn the
 *   driving voltages off again.  Called with the SPI bus held, and returns
 *   with it held, but lets go of it while the panel works: a refresh takes
 *   most of a second, the panel does not need the bus for it, and the
 *   microSD card and the radio on the same bus would otherwise be locked
 *   out for all of it.
 *
 ****************************************************************************/

static int uc8253_finish(FAR struct uc8253_dev_s *priv, bool partial)
{
  int ret;

  uc8253_unlock(priv);
  ret = uc8253_busywait(priv, UC8253_REFRESH_TMO_MS);
  uc8253_lock(priv);

#ifdef CONFIG_LCD_UC8253_PARTIAL
  if (partial)
    {
      uc8253_sendcmd(priv, UC8253_PTOUT);
    }
#endif

  uc8253_poweroff(priv);
  uc8253_softreset(priv);
  return ret;
}

/****************************************************************************
 * Name: uc8253_clear
 *
 * Description:
 *   Drive the whole panel to white.  Both controller buffers are written,
 *   so this also gives the "previous" buffer a defined value, which every
 *   later differential refresh depends on.  Called with priv->panel held.
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

  ret = uc8253_start(priv, false);
  if (ret >= 0)
    {
      ret = uc8253_finish(priv, false);
    }
  else
    {
      uc8253_poweroff(priv);
    }

  uc8253_unlock(priv);

  if (ret >= 0)
    {
      memset(priv->glass_fb, 0xff, UC8253_FBSIZE);
      priv->initial  = false;
      priv->known    = true;
      priv->stale    = false;
      priv->partials = 0;
    }

  return ret;
}

/****************************************************************************
 * Name: uc8253_reseed
 *
 * Description:
 *   Load both controller buffers with what the glass shows, without
 *   refreshing.  This is how the panel comes back from deep sleep: the
 *   controller has lost its RAM, but the glass, being bistable, still shows
 *   what it did.  Anything drawn since goes out with the next refresh.
 *   Called with priv->panel held.
 *
 ****************************************************************************/

static int uc8253_reseed(FAR struct uc8253_dev_s *priv)
{
  uc8253_lock(priv);

  uc8253_sendcmd(priv, UC8253_DTM1);
  uc8253_senddata(priv, priv->glass_fb, UC8253_FBSIZE);
  uc8253_sendcmd(priv, UC8253_DTM2);
  uc8253_senddata(priv, priv->glass_fb, UC8253_FBSIZE);

  uc8253_unlock(priv);

  priv->initial = false;
  return OK;
}

/****************************************************************************
 * Name: uc8253_update
 *
 * Description:
 *   Bring the panel up to date with the shadow framebuffer: push whatever
 *   has been drawn since the last refresh and refresh the part of the panel
 *   it covers.
 *
 *   A partial refresh drives each pixel from the transition between the
 *   controller's "previous" and "current" buffers, so both are written for
 *   the rectangle: the previous one with what the glass shows, from
 *   glass_fb, and the current one with the new drawing.  A full refresh
 *   drives every pixel regardless and, as in the vendor's code, gets the new
 *   frame in both.
 *
 *   Refreshes are serialised by priv->panel.  The framebuffer lock is only
 *   held while the dirty rows are copied out, so drawing can carry on while
 *   the panel refreshes; whatever is drawn meanwhile is picked up by the
 *   next update.
 *
 ****************************************************************************/

static int uc8253_update(FAR struct uc8253_dev_s *priv)
{
  fb_coord_t x;
  fb_coord_t y;
  fb_coord_t w;
  fb_coord_t h;
  fb_coord_t row;
  bool partial = false;
  int ret = OK;

  nxmutex_lock(&priv->panel);

  if (!priv->on)
    {
      /* Nothing is lost: the drawing stays in the shadow framebuffer and
       * goes out with the first update after the panel is powered again.
       */

      goto out;
    }

  if (!priv->configured)
    {
      ret = uc8253_configure(priv);
      if (ret < 0)
        {
          goto out;
        }
    }

  if (priv->initial)
    {
      ret = priv->known ? uc8253_reseed(priv) : uc8253_clear(priv);
      if (ret < 0)
        {
          goto out;
        }
    }

  nxmutex_lock(&priv->fblock);

  if (priv->x1 > priv->x2)
    {
      /* Nothing has been drawn since the last refresh.  Refreshing anyway
       * would cost a second and wear the panel for no reason.
       */

      nxmutex_unlock(&priv->fblock);
      goto out;
    }

  /* The controller addresses its RAM by the byte horizontally, so widen the
   * rectangle to byte boundaries.
   */

  x = priv->x1 & ~0x0007;
  y = priv->y1;
  w = ((priv->x2 | 0x0007) - x) + 1;
  h = (priv->y2 - y) + 1;

  /* After a failed refresh glass_fb cannot be trusted to tell what needs
   * redrawing, and the full refresh that follows redraws everything anyway.
   */

  if (!priv->stale && !uc8253_trim(priv, &x, &y, &w, &h))
    {
      uc8253_cleandirty(priv);
      nxmutex_unlock(&priv->fblock);
      goto out;
    }

#ifdef CONFIG_LCD_UC8253_PARTIAL
  /* A partial refresh leaves the rest of the screen alone, so it is worth
   * using whenever the change does not cover the whole panel.  It does
   * leave a little ghosting behind, so a full refresh is forced every so
   * often to clean it up.
   */

  partial = !priv->stale &&
            (x > 0 || y > 0 || w < UC8253_XRES || h < UC8253_YRES) &&
            (CONFIG_LCD_UC8253_FULL_EVERY == 0 ||
             priv->partials < CONFIG_LCD_UC8253_FULL_EVERY);
#endif

  uc8253_cleandirty(priv);
  uc8253_lock(priv);

#ifdef CONFIG_LCD_UC8253_PARTIAL
  if (partial)
    {
      /* Previous frame first, while glass_fb still holds it, then the new
       * rectangle, then select the same window for the refresh.
       */

      uc8253_sendwindow(priv, UC8253_DTM1, x, y, w, h);

      for (row = y; row < y + h; row++)
        {
          memcpy(priv->glass_fb + row * UC8253_ROWSIZE + (x >> 3),
                 priv->shadow_fb + row * UC8253_ROWSIZE + (x >> 3),
                 w >> 3);
        }

      nxmutex_unlock(&priv->fblock);

      uc8253_sendwindow(priv, UC8253_DTM2, x, y, w, h);
      uc8253_sendcmd(priv, UC8253_PTIN);
      uc8253_setwindow(priv, x, y, w, h);
    }
  else
#endif
    {
      UNUSED(row);
      memcpy(priv->glass_fb, priv->shadow_fb, UC8253_FBSIZE);
      nxmutex_unlock(&priv->fblock);

      uc8253_sendcmd(priv, UC8253_DTM1);
      uc8253_senddata(priv, priv->glass_fb, UC8253_FBSIZE);
      uc8253_sendcmd(priv, UC8253_DTM2);
      uc8253_senddata(priv, priv->glass_fb, UC8253_FBSIZE);
    }

  ret = uc8253_start(priv, partial);
  if (ret >= 0)
    {
      ret = uc8253_finish(priv, partial);
    }
  else
    {
#ifdef CONFIG_LCD_UC8253_PARTIAL
      if (partial)
        {
          uc8253_sendcmd(priv, UC8253_PTOUT);
        }
#endif

      uc8253_poweroff(priv);
      uc8253_softreset(priv);
    }

  uc8253_unlock(priv);

  if (ret < 0)
    {
      /* Keep the change, so that the next update tries again.  What the
       * glass shows is in doubt now, and a partial refresh would take
       * glass_fb at its word, so make that a full one.
       */

      priv->stale = true;
      nxmutex_lock(&priv->fblock);
      uc8253_dirty(priv, x, y, x + w - 1, y + h - 1);
      nxmutex_unlock(&priv->fblock);
    }
  else if (partial)
    {
      priv->partials++;
    }
  else
    {
      priv->partials = 0;
      priv->stale    = false;
    }

out:
  nxmutex_unlock(&priv->panel);
  return ret;
}

#ifdef CONFIG_LCD_UC8253_ASYNC

/****************************************************************************
 * Name: uc8253_kick
 *
 * Description:
 *   Wake the refresh thread.  One pending wake-up is enough: the refresh
 *   picks up everything drawn up to the moment it copies the dirty rows out.
 *
 ****************************************************************************/

static void uc8253_kick(FAR struct uc8253_dev_s *priv)
{
  int count;

  nxsem_get_value(&priv->kick, &count);
  if (count < 1)
    {
      nxsem_post(&priv->kick);
    }
}

/****************************************************************************
 * Name: uc8253_thread
 *
 * Description:
 *   Refresh the panel whenever redraw() asks for it.  An e-paper refresh
 *   takes most of a second, and a caller drawing a little at a time (a
 *   terminal draws a glyph, then moves its cursor) would otherwise wait out
 *   one refresh per call.  Here each call only records that there is work;
 *   the thread waits briefly for a burst of drawing to finish and pushes
 *   all of it in one refresh.  Anything drawn while that refresh runs rides
 *   along with the next one.
 *
 ****************************************************************************/

static int uc8253_thread(int argc, FAR char *argv[])
{
  FAR struct uc8253_dev_s *priv = &g_epaperdev;

  for (; ; )
    {
      nxsem_wait_uninterruptible(&priv->kick);

#if CONFIG_LCD_UC8253_ASYNC_DELAY > 0
      nxsched_usleep(CONFIG_LCD_UC8253_ASYNC_DELAY * 1000);
#endif

      uc8253_update(priv);
    }

  return OK;
}

#endif /* CONFIG_LCD_UC8253_ASYNC */

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

  nxmutex_lock(&priv->fblock);
  uc8253_bitcpy(priv->shadow_fb + row * UC8253_ROWSIZE + (col >> 3),
                col & 7, buffer, npixels);
  uc8253_dirty(priv, col, row, col + npixels - 1, row);
  nxmutex_unlock(&priv->fblock);

#ifdef CONFIG_LCD_UC8253_AUTOREFRESH
  uc8253_kick(priv);
#endif

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

  nxmutex_lock(&priv->fblock);

  for (row = row_start; row <= row_end; row++)
    {
      FAR uint8_t *dst = priv->shadow_fb + row * UC8253_ROWSIZE +
                         (col_start >> 3);

      uc8253_bitcpy(dst, col_start & 7,
                    buffer + (row - row_start) * stride, npixels);
    }

  uc8253_dirty(priv, col_start, row_start, col_end, row_end);
  nxmutex_unlock(&priv->fblock);

#ifdef CONFIG_LCD_UC8253_AUTOREFRESH
  uc8253_kick(priv);
#endif

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

  nxmutex_lock(&priv->fblock);
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

  nxmutex_unlock(&priv->fblock);
  return OK;
}

/****************************************************************************
 * Name: uc8253_redraw
 ****************************************************************************/

static int uc8253_redraw(FAR struct lcd_dev_s *dev)
{
  FAR struct uc8253_dev_s *priv = (FAR struct uc8253_dev_s *)dev;

  if (!priv->on)
    {
      lcdwarn("WARNING: Refusing to redraw a powered down panel\n");
      return -EPERM;
    }

  nxmutex_lock(&priv->fblock);

  if (!priv->known && !priv->dropped)
    {
      /* The LCD framebuffer front end flushes its framebuffer as soon as it
       * registers, and that buffer has just been allocated and zeroed, which
       * on this panel means every pixel black.  Nobody asked for a black
       * screen, so the first redraw after a reset is taken as a clear: the
       * panel is driven to white and that frame is dropped.  The shadow is
       * set to white as well, so that it keeps matching the glass; a later
       * full refresh would otherwise bring the dropped frame back.
       */

      uc8253_cleandirty(priv);
      memset(priv->shadow_fb, 0xff, UC8253_FBSIZE);
      priv->dropped = true;
    }

  nxmutex_unlock(&priv->fblock);

#ifdef CONFIG_LCD_UC8253_ASYNC
  uc8253_kick(priv);
  return OK;
#else
  return uc8253_update(priv);
#endif
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

  int ret = OK;

  /* Wait for any refresh in progress, so the panel is never put to sleep
   * half way through one.
   */

  nxmutex_lock(&priv->panel);

  if (power > 0)
    {
      if (!priv->configured)
        {
          ret = uc8253_configure(priv);
        }

      if (ret >= 0)
        {
          priv->on = true;
        }
    }
  else if (priv->on)
    {
      /* Deep sleep needs the check code, and only takes effect once the
       * driving voltages are off.  The controller loses its RAM, but the
       * glass keeps its image, so on wake-up the controller is reseeded
       * from the shadow framebuffer rather than cleared.
       */

      uc8253_lock(priv);
      uc8253_poweroff(priv);
      uc8253_sendcmd1(priv, UC8253_DEEP_SLEEP, UC8253_DEEP_SLEEP_CHECK);
      uc8253_unlock(priv);

      priv->on         = false;
      priv->configured = false;
    }

  nxmutex_unlock(&priv->panel);
  return ret;
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
  priv->known      = false;
  priv->dropped    = false;
  priv->stale      = false;
  priv->partials   = 0;

  nxmutex_init(&priv->panel);
  nxmutex_init(&priv->fblock);

  uc8253_cleandirty(priv);

  /* Start from a white screen.  A set bit is white on this panel. */

  memset(priv->shadow_fb, 0xff, UC8253_FBSIZE);
  memset(priv->glass_fb, 0xff, UC8253_FBSIZE);

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

#ifdef CONFIG_LCD_UC8253_ASYNC
  nxsem_init(&priv->kick, 0, 0);

  ret = kthread_create("uc8253", CONFIG_LCD_UC8253_THREAD_PRIORITY,
                       CONFIG_LCD_UC8253_THREAD_STACKSIZE, uc8253_thread,
                       NULL);
  if (ret < 0)
    {
      lcderr("ERROR: Failed to start the refresh thread: %d\n", ret);
      return NULL;
    }
#endif

  lcdinfo("UC8253 ready: %dx%d, %d bpp\n",
          UC8253_XRES, UC8253_YRES, UC8253_BPP);

  return &priv->dev;
}

#endif /* CONFIG_LCD_UC8253 */
