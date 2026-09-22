/****************************************************************************
 * include/nuttx/input/cst3530.h
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

#ifndef __INCLUDE_NUTTX_INPUT_CST3530_H
#define __INCLUDE_NUTTX_INPUT_CST3530_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>

#ifdef CONFIG_INPUT_CST3530

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The CST3530 answers at this 7-bit I2C address */

#define CST3530_I2C_ADDRESS 0x1a

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board hooks.  The structure must stay valid for as long as the driver is
 * registered.
 */

struct cst3530_config_s
{
  uint32_t frequency;  /* I2C frequency */
  uint8_t  flags;      /* TOUCH_FLAG_SWAPXY / MIRRORX / MIRRORY */

  /* Attach isr to the interrupt line, active low: the controller pulls it
   * down for each new report.  It may be edge or level triggered; the
   * driver masks it while it reads a report.  The interrupt is left
   * disabled.
   */

  CODE int  (*attach)(FAR const struct cst3530_config_s *config,
                      xcpt_t isr, FAR void *arg);

  /* Enable or disable the interrupt */

  CODE void (*enable)(FAR const struct cst3530_config_s *config,
                      bool enable);

  /* Drive the reset line: true holds the controller in reset */

  CODE void (*reset)(FAR const struct cst3530_config_s *config,
                     bool assert);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

struct i2c_master_s;

/****************************************************************************
 * Name: cst3530_register
 *
 * Description:
 *   Register a Hynitron CST3530 capacitive touch controller, or a CST66xx
 *   that speaks the same protocol, as a touchscreen device.  The resolution
 *   is read from the controller.
 *
 *   The controller is kept in deep sleep while nobody has the device open:
 *   opening it resets the controller into normal operation and closing it
 *   puts it back to sleep.
 *
 * Input Parameters:
 *   devpath - The device to register, e.g. "/dev/input0"
 *   i2c     - An instance of the I2C interface to use
 *   addr    - The I2C address, normally CST3530_I2C_ADDRESS
 *   config  - Board hooks, kept by the driver
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure, -ENODEV if no
 *   controller of this family answers.
 *
 ****************************************************************************/

int cst3530_register(FAR const char *devpath, FAR struct i2c_master_s *i2c,
                     uint8_t addr,
                     FAR const struct cst3530_config_s *config);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* CONFIG_INPUT_CST3530 */
#endif /* __INCLUDE_NUTTX_INPUT_CST3530_H */
