/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_haptic.c
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
 * The vibration motor is an ERM driven by a DRV2605L on the shared I2C
 * bus.  The chip's enable is the XL9555's motor_en line, which the expander
 * setup turns on before this runs.  The vendor's firmware uses effect
 * library 1 (ERM library A), and so does this.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/drv2605.h>

#include "esp32s3_i2c.h"

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_FF_DRV2605

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TDECKMAX_HAPTIC_I2C_FREQUENCY 400000
#define TDECKMAX_HAPTIC_LIBRARY       1

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_haptic_initialize
 *
 * Description:
 *   Register the vibration motor as /dev/input_ff0.
 *
 ****************************************************************************/

int tdeckmax_haptic_initialize(void)
{
  FAR struct i2c_master_s *i2c;

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  return drv2605_register("/dev/input_ff0", i2c, DRV2605_I2C_ADDRESS,
                          TDECKMAX_HAPTIC_I2C_FREQUENCY,
                          TDECKMAX_HAPTIC_LIBRARY);
}

#endif /* CONFIG_FF_DRV2605 */
