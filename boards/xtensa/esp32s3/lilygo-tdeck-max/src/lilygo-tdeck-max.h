/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/lilygo-tdeck-max.h
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

#ifndef __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_SRC_LILYGO_TDECK_MAX_H
#define __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_SRC_LILYGO_TDECK_MAX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>
#include <stdint.h>

#include <arch/board/board.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Every I2C peripheral on the board shares one bus */

#define TDECKMAX_I2C_PORT 0

/****************************************************************************
 * Public Types
 ****************************************************************************/

#ifndef __ASSEMBLY__

struct ioexpander_dev_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_bringup
 *
 * Description:
 *   Perform architecture-specific initialization
 *
 *   CONFIG_BOARD_LATE_INITIALIZE=y :
 *     Called from board_late_initialize().
 *
 ****************************************************************************/

int esp32s3_bringup(void);

/****************************************************************************
 * Name: board_i2c_init
 *
 * Description:
 *   Configure and register the I2C drivers (common board code).
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_I2C_DRIVER
int board_i2c_init(void);
#endif

/****************************************************************************
 * Name: tdeckmax_xl9555_initialize
 *
 * Description:
 *   Bring up the XL9555 I/O expander, which gates the power of most
 *   peripherals on the T-Deck Max, put every controlled rail in its default
 *   state and register each line as /dev/<name> (see esp32s3_xl9555.c).
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_IOEXPANDER_PCA9555
int tdeckmax_xl9555_initialize(void);

/****************************************************************************
 * Name: tdeckmax_xl9555_get
 *
 * Description:
 *   Return the I/O expander instance created by
 *   tdeckmax_xl9555_initialize(), or NULL if it is not available.  Other
 *   drivers use it to reach the touch, keyboard and audio control lines.
 *
 ****************************************************************************/

struct ioexpander_dev_s *tdeckmax_xl9555_get(void);
#endif

/****************************************************************************
 * Name: tdeckmax_keyboard_initialize
 *
 * Description:
 *   Bring up the TCA8418 behind the T-Deck Max keyboard and register it as
 *   /dev/kbd0.  The XL9555 must already have released the keyboard's reset,
 *   so this runs after tdeckmax_xl9555_initialize().
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_INPUT_TCA8418
int tdeckmax_keyboard_initialize(void);
#endif

/****************************************************************************
 * Name: tdeckmax_battery_initialize
 *
 * Description:
 *   Bring up the BQ27220 fuel gauge and the SY6970 charger, set the
 *   battery's capacity and charge limits, and register them as /dev/batt0
 *   and /dev/charger0.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#if defined(CONFIG_BQ27220) || defined(CONFIG_SY6970)
int tdeckmax_battery_initialize(void);
#endif

/****************************************************************************
 * Name: tdeckmax_radios_initialize
 *
 * Description:
 *   Register /dev/wifi_en and /dev/ble_en, through which a radio is brought
 *   up at runtime, and bring up the ones the board is configured to have
 *   running at boot (LILYGO_TDECK_MAX_BOOT_WIFI, LILYGO_TDECK_MAX_BOOT_BLE).
 *   The radios are otherwise left alone: see esp32s3_radios.c for why.
 *
 * Returned Value:
 *   Zero (OK).  Failures to bring a radio up are logged.
 *
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_WIRELESS
int tdeckmax_radios_initialize(void);

/****************************************************************************
 * Name: tdeckmax_wifi_enable / tdeckmax_ble_enable
 *
 * Description:
 *   Bring a radio up if it is not up already.  There is no way back down.
 *   Wi-Fi registers wlan0 and BLE registers bnep0; the Wi-Fi radio itself
 *   only starts when wlan0 is brought up.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned
 *   to indicate the nature of any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_WIFI
int tdeckmax_wifi_enable(void);
#endif

#ifdef CONFIG_ESPRESSIF_BLE
int tdeckmax_ble_enable(void);
#endif
#endif /* CONFIG_ESPRESSIF_WIRELESS */

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_SRC_LILYGO_TDECK_MAX_H */
