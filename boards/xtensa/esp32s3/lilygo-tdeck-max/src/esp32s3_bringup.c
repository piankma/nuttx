/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_bringup.c
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

#include <errno.h>
#include <syslog.h>

#include <nuttx/debug.h>
#include <nuttx/fs/fs.h>
#include <arch/board/board.h>

#ifdef CONFIG_ESPRESSIF_HR_TIMER
#  include "espressif/esp_hr_timer.h"
#endif

#ifdef CONFIG_ESP32S3_SPI
#  include "esp32s3_spi.h"
#  include "esp32s3_board_spidev.h"
#endif

#ifdef CONFIG_MMCSD_SPI
#  include "esp32s3_board_sdmmc.h"
#endif

#ifdef CONFIG_LCD_UC8253
#  include <nuttx/board.h>
#  include <nuttx/lcd/lcd.h>
#  ifdef CONFIG_LCD_DEV
#    include <nuttx/lcd/lcd_dev.h>
#  endif
#  ifdef CONFIG_VIDEO_FB
#    include <nuttx/video/fb.h>
#  endif
#endif

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization.
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 ****************************************************************************/

int esp32s3_bringup(void)
{
  int ret;

#ifdef CONFIG_ESPRESSIF_HR_TIMER
  ret = esp_hr_timer_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_hr_timer_init() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the tmpfs file system */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to mount tmpfs at %s: %d\n",
             CONFIG_LIBC_TMPDIR, ret);
    }
#endif

#ifdef CONFIG_I2C_DRIVER
  /* Register /dev/i2c0 (shared bus: expander, touch, keyboard, IMU, ...) */

  ret = board_i2c_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize I2C driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_IOEXPANDER_PCA9555
  /* The XL9555 gates the power of most peripherals, so it comes first */

  ret = tdeckmax_xl9555_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize XL9555: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESP32S3_SPI2) && defined(CONFIG_SPI_DRIVER)
  /* Register /dev/spi2 (shared bus: e-paper, microSD, SX1262) */

  ret = board_spidev_initialize(ESP32S3_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize spidev%d: %d\n",
             ESP32S3_SPI2, ret);
    }
#endif

#if defined(CONFIG_ESP32S3_SPI2) && \
    defined(CONFIG_LILYGO_TDECK_MAX_BOOT_LORA_POWER)
  /* The LoRa rail is on: put the radio to sleep until something uses it */

  ret = tdeckmax_lora_sleep();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to put the SX1262 to sleep: %d\n", ret);
    }
#endif

#ifdef CONFIG_MMCSD_SPI
  /* Register /dev/mmcsd0 for the microSD slot */

  ret = board_sdmmc_spi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize microSD: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_TCA8418
  /* Register /dev/kbd0.  The XL9555 has already released the keyboard's
   * reset line above, which the scanner needs before it will answer.
   */

  ret = tdeckmax_keyboard_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize the keyboard: %d\n", ret);
    }
#endif

#if defined(CONFIG_BQ27220) || defined(CONFIG_SY6970)
  /* Register /dev/batt0 (fuel gauge) and /dev/charger0 (charger) */

  ret = tdeckmax_battery_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize the battery: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESP32S3_AUTO_SLEEP) && !defined(CONFIG_SY6970)
  /* Only the charger can tell whether USB is connected, and light sleep
   * would disconnect a USB console.  Without it, never sleep.
   */

  esp32s3_sleep_hold();
#endif

#ifdef CONFIG_FF_DRV2605
  /* Register /dev/input_ff0, the vibration motor */

  ret = tdeckmax_haptic_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize the motor: %d\n", ret);
    }
#endif

#ifdef CONFIG_INPUT_CST3530
  /* Register /dev/input0, the touch panel */

  ret = tdeckmax_touch_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize touch: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESPRESSIF_LEDC) && defined(CONFIG_PWM)
  /* Register /dev/pwm0.  Channel 1 is the e-paper frontlight and channel 2
   * is the keyboard backlight; both are off until something drives them.
   */

  ret = tdeckmax_pwm_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize PWM: %d\n", ret);
    }
#endif

#ifdef CONFIG_LCD_UC8253
  /* Bring up the e-paper panel.  This resets and configures the controller
   * but deliberately leaves the glass showing whatever was already on it:
   * a refresh takes about a second, and making every boot pay for one would
   * be wrong for a handheld.  The first FBIO_UPDATE starts from white.
   */

  ret = board_lcd_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize the e-paper: %d\n", ret);
    }
  else
    {
#ifdef CONFIG_LCD_DEV
      ret = lcddev_register(0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/lcd0: %d\n", ret);
        }
#endif

#ifdef CONFIG_VIDEO_FB
      ret = fb_register(0, 0);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: Failed to register /dev/fb0: %d\n", ret);
        }
#endif
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIRELESS
  /* Offer the radios as devices and bring up any the board is configured to
   * have running at boot.  By default neither is: see esp32s3_radios.c.
   */

  ret = tdeckmax_radios_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize the radios: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  /* Every wake-up source and hold is in place: let the chip sleep */

  esp32s3_sleep_release();
#endif

  UNUSED(ret);
  return OK;
}
