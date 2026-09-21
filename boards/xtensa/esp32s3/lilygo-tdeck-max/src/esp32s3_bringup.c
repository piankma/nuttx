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

#ifdef CONFIG_ESPRESSIF_WIFI
#  include "esp32s3_board_wlan.h"
#endif

#ifdef CONFIG_ESPRESSIF_BLE
#  include "esp32s3_ble.h"
#endif

#ifdef CONFIG_ESPRESSIF_WIFI_BT_COEXIST
#  include "esp32s3_wifi_adapter.h"
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

#ifdef CONFIG_MMCSD_SPI
  /* Register /dev/mmcsd0 for the microSD slot */

  ret = board_sdmmc_spi_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize microSD: %d\n", ret);
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
  /* The radios share one antenna and one set of low-level resources, so the
   * coexistence arbiter has to be started before either of them.
   */

#ifdef CONFIG_ESPRESSIF_WIFI_BT_COEXIST
  ret = esp_wifi_bt_coexist_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init Wi-Fi/BT coexistence: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_BLE
  ret = esp32s3_ble_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize BLE: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_WIFI
  /* Registers the wlan0 network device (CONFIG_NETDEV_LATEINIT) */

  ret = board_wlan_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize Wi-Fi: %d\n", ret);
    }
#endif

#endif /* CONFIG_ESPRESSIF_WIRELESS */

  UNUSED(ret);
  return OK;
}
