/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_board_spi.c
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

#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <nuttx/debug.h>
#include <nuttx/spi/spi.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "esp32s3_spi.h"

#ifdef CONFIG_ESP32S3_SPI2

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_spi2_csnum
 *
 * Description:
 *   Map an SPI device ID to the GPIO used as its chip select.
 *
 * Returned Value:
 *   The GPIO number or -1 if the device ID is not known.
 *
 ****************************************************************************/

static int tdeckmax_spi2_csnum(uint32_t devid)
{
  if (devid == SPIDEV_MMCSD(0))
    {
      return BOARD_SD_CS;
    }
  else if (devid == SPIDEV_DISPLAY(0))
    {
      return BOARD_EPD_CS;
    }
  else if (devid == SPIDEV_LPWAN(0))
    {
      return BOARD_LORA_CS;
    }

  return -1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_spi2_select
 *
 * Description:
 *   Drive the chip select of one of the three devices sharing SPI2.
 *   The driver also calls this with its own bus identifier while it
 *   initializes; in that case (and for any unknown device) all the chip
 *   selects are released.
 *
 ****************************************************************************/

void esp32s3_spi2_select(struct spi_dev_s *dev, uint32_t devid,
                         bool selected)
{
  int cs = tdeckmax_spi2_csnum(devid);

  spiinfo("devid: %08" PRIx32 " CS: %s\n",
          devid, selected ? "select" : "free");

  if (cs >= 0)
    {
      esp_gpiowrite(cs, !selected);
    }
  else if (!selected)
    {
      esp_gpiowrite(BOARD_SD_CS, true);
      esp_gpiowrite(BOARD_EPD_CS, true);
      esp_gpiowrite(BOARD_LORA_CS, true);
    }
}

/****************************************************************************
 * Name: esp32s3_spi2_status
 *
 * Description:
 *   The microSD slot has no card-detect switch, so it is always reported
 *   as present and the MMC/SD driver probes the card itself.
 *
 ****************************************************************************/

uint8_t esp32s3_spi2_status(struct spi_dev_s *dev, uint32_t devid)
{
  if (devid == SPIDEV_MMCSD(0))
    {
      return SPI_STATUS_PRESENT;
    }

  return 0;
}

/****************************************************************************
 * Name: esp32s3_spi2_cmddata
 *
 * Description:
 *   The e-paper D/C line: low selects a command, high selects data.
 *
 *   The SPI transfer layer calls this before every transfer of every
 *   device, and gives up if it fails, so it must succeed for the devices
 *   that have no D/C line (microSD, SX1262) as long as the transfer is a
 *   plain data phase.  Only a command phase is meaningless for them.
 *
 ****************************************************************************/

#ifdef CONFIG_SPI_CMDDATA
int esp32s3_spi2_cmddata(struct spi_dev_s *dev, uint32_t devid, bool cmd)
{
  if (devid == SPIDEV_DISPLAY(0))
    {
      esp_gpiowrite(BOARD_EPD_DC, !cmd);
      return OK;
    }

  spiinfo("devid: %08" PRIx32 " CMD: %s\n", devid, cmd ? "command" : "data");
  return cmd ? -ENODEV : OK;
}
#endif

#endif /* CONFIG_ESP32S3_SPI2 */
