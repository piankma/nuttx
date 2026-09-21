/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/include/board.h
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

#ifndef __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_INCLUDE_BOARD_H
#define __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_INCLUDE_BOARD_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clocking *****************************************************************/

/* The T-Deck Max is fitted with a 40MHz crystal */

#define BOARD_XTAL_FREQUENCY    40000000

#ifdef CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ
#  define BOARD_CLOCK_FREQUENCY (CONFIG_ESP32S3_DEFAULT_CPU_FREQ_MHZ * 1000000)
#else
#  define BOARD_CLOCK_FREQUENCY 80000000
#endif

/* Pin map ******************************************************************
 *
 * Source: LilyGo T-Deck-MAX repository (docs/pinmap.md and
 * lib/TDeckMaxBoard/src/TDeckMaxBoard.h), schematic V0.1 25-09-11.
 *
 * NAMING WARNING: the vendor macros mix two viewpoints.  The names below
 * are always from the ESP32-S3 point of view ("TX" is a pin the ESP32-S3
 * drives).  In the vendor sources the GPS macros are already ESP32-side,
 * but the A7682E macros are modem-side: BOARD_A7682E_RXD (GPIO10) is the
 * modem's RXD input, so it is the ESP32-S3 TX pin.  The vendor's working
 * code confirms this: SerialAT.begin(115200, SERIAL_8N1,
 * BOARD_A7682E_TXD, BOARD_A7682E_RXD) passes (rx, tx) in that order.
 *
 * Hardware status (see the board documentation): reception from the GPS and
 * from the modem was verified on real hardware; transmission to the modem
 * has not been verified.
 */

/* Shared I2C bus (I2C0) */

#define BOARD_I2C_SDA           13
#define BOARD_I2C_SCL           14

/* I2C addresses (7-bit) */

#define BOARD_I2C_ADDR_ES8311   0x18  /* Audio codec */
#define BOARD_I2C_ADDR_TOUCH    0x1a  /* CST3530 touch controller */
#define BOARD_I2C_ADDR_XL9555   0x20  /* I/O expander */
#define BOARD_I2C_ADDR_BHI260AP 0x28  /* IMU (needs 1V8_EN) */
#define BOARD_I2C_ADDR_TCA8418  0x34  /* Keyboard matrix controller */
#define BOARD_I2C_ADDR_BQ27220  0x55  /* Fuel gauge */
#define BOARD_I2C_ADDR_DRV2605  0x5a  /* Haptic motor driver (needs M_EN) */
#define BOARD_I2C_ADDR_SY6970   0x6a  /* Charger (older boards: BQ25896 0x6b) */

/* Shared SPI bus (SPI2): e-paper, microSD and SX1262 LoRa.  Each device has
 * its own chip select; all chip selects must idle high before any of the
 * devices is powered or accessed.
 */

#define BOARD_SPI_SCK           36
#define BOARD_SPI_MOSI          33
#define BOARD_SPI_MISO          47

#define BOARD_EPD_CS            34
#define BOARD_SD_CS             48
#define BOARD_LORA_CS           3

/* GDEQ031T10 e-paper (UC8253 controller, 240x320, write-only) */

#define BOARD_EPD_DC            35
#define BOARD_EPD_BUSY          37
#define BOARD_EPD_RST           9
#define BOARD_EPD_FRONTLIGHT    41    /* BL_PWM */

/* SX1262 LoRa (power and antenna switch are on the XL9555) */

#define BOARD_LORA_RST          4     /* NRESET */
#define BOARD_LORA_DIO1         5     /* IRQ */
#define BOARD_LORA_BUSY         6

/* MIA-M10Q GPS on UART1 (ESP32-S3 side names), 38400 baud default */

#define BOARD_GPS_UART_TX       16    /* ESP32-S3 TX -> GPS RX */
#define BOARD_GPS_UART_RX       2     /* GPS TX -> ESP32-S3 RX */
#define BOARD_GPS_PPS           1

/* A7682E 4G modem on UART2 (ESP32-S3 side names), 115200 baud default */

#define BOARD_MODEM_UART_TX     10    /* ESP32-S3 TX -> modem RXD */
#define BOARD_MODEM_UART_RX     11    /* modem TXD -> ESP32-S3 RX */
#define BOARD_MODEM_RI          7
#define BOARD_MODEM_DTR         8

/* Input devices and misc */

#define BOARD_TOUCH_INT         12
#define BOARD_KEYBOARD_INT      15    /* TCA8418 INT */
#define BOARD_KEYBOARD_BACKLIGHT 42   /* LED_PWM */
#define BOARD_IMU_INT           21    /* BHI260AP HIRQ */
#define BOARD_BOOT_BUTTON       0

/* ES8311 audio codec (I2S, ESP32-S3 side names) */

#define BOARD_I2S_MCLK          38
#define BOARD_I2S_BCLK          39
#define BOARD_I2S_WS            18
#define BOARD_I2S_DOUT          17    /* ESP32-S3 DOUT -> ES8311 DSDIN */
#define BOARD_I2S_DIN           40    /* ES8311 ASDOUT -> ESP32-S3 DIN */

/* XL9555 I/O expander (PCA9555 compatible, address 0x20, INT not wired).
 * Pin numbers follow the PCA9555 driver: 0-7 are P00-P07, 8-15 are P10-P17.
 */

#define XL9555_PIN_MODEM_PWR    0     /* P00 6609_EN: HIGH powers the A7682E */
#define XL9555_PIN_LORA_EN      1     /* P01 LORA_EN: HIGH powers the SX1262 */
#define XL9555_PIN_GPS_EN       2     /* P02 GPS_EN: HIGH powers the GPS */
#define XL9555_PIN_IMU_1V8_EN   3     /* P03 1V8_EN: HIGH powers the BHI260AP */
#define XL9555_PIN_LORA_ANT     4     /* P04 LORA_SEL: HIGH internal antenna */
#define XL9555_PIN_MOTOR_EN     5     /* P05 M_EN: HIGH powers the DRV2605 */
#define XL9555_PIN_AMP_EN       6     /* P06 SHUTDOWM: HIGH enables the amp */
#define XL9555_PIN_TOUCH_RST    7     /* P07 T_RST: LOW resets the touch IC */
#define XL9555_PIN_MODEM_PWRKEY 8     /* P10 PWRKEY_EN: HIGH asserts PWRKEY */
#define XL9555_PIN_KEY_RST      9     /* P11 KEY_RST: LOW resets keyboard IC */
#define XL9555_PIN_AUDIO_SEL    10    /* P12 AUDIO_SEL: HIGH modem, LOW codec */

#endif /* __BOARDS_XTENSA_ESP32S3_LILYGO_TDECK_MAX_INCLUDE_BOARD_H */
