/****************************************************************************
 * include/nuttx/input/tca8418.h
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

#ifndef __INCLUDE_NUTTX_INPUT_TCA8418_H
#define __INCLUDE_NUTTX_INPUT_TCA8418_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/irq.h>

#ifdef CONFIG_INPUT_TCA8418

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The controller scans up to 8 rows by 10 columns */

#define TCA8418_MAX_ROWS    8
#define TCA8418_MAX_COLS    10

/* Keymap entries.
 *
 * A keymap is a flat array of rows * cols entries indexed as
 * [row * cols + col].  Each entry is one of:
 *
 *   TCA8418_NONE            nothing is wired at this position
 *   0x01 .. 0xff            a character, reported as a normal key
 *   TCA8418_SHIFT/ALT/SYM   a modifier, handled inside the driver and
 *                           never reported
 *   TCA8418_SPEC(keycode)   a special key, reported with a value from
 *                           enum kbd_keycode_e
 *
 * Which layer a modifier selects is fixed by the driver, but what each
 * layer contains is entirely up to the board:
 *
 *   SHIFT  held      selects the shift layer
 *   SYM    held      selects the symbol layer
 *   ALT    pressed   toggles the shift layer on and off, like caps lock
 *
 * SYM wins over SHIFT when both are held.
 */

#define TCA8418_NONE        0x0000
#define TCA8418_SHIFT       0x0100
#define TCA8418_ALT         0x0101
#define TCA8418_SYM         0x0102
#define TCA8418_SPEC(k)     (0x0200 + (k))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A reference to a structure of this type must be passed to the TCA8418
 * driver.  It describes the keypad matrix and hides the board specific
 * parts of interrupt handling.
 */

struct tca8418_config_s
{
  /* Device characterization */

  uint32_t frequency;  /* I2C frequency */
  uint8_t  address;    /* I2C 7-bit device address */
  uint8_t  rows;       /* Rows in use, 1 to TCA8418_MAX_ROWS */
  uint8_t  cols;       /* Columns in use, 1 to TCA8418_MAX_COLS */

  /* True if column 0 of the keymap is the rightmost column of the matrix.
   * Keyboards are often wired this way because it shortens the traces, and
   * it is easier to flip the lookup here than to write the keymap backwards.
   */

  bool     colreverse;

  /* Layer keymaps, each rows * cols entries.  "base" is required.  "shift"
   * and "sym" may be NULL, in which case that modifier falls back to the
   * base layer.
   */

  FAR const uint16_t *base;
  FAR const uint16_t *shift;
  FAR const uint16_t *sym;

  /* IRQ/GPIO access callbacks.  These operations are all hidden behind
   * callbacks to isolate the driver from differences in GPIO interrupt
   * handling between boards and MCUs.
   *
   * attach  - Attach the interrupt handler to the GPIO interrupt
   * enable  - Enable or disable the GPIO interrupt
   * clear   - Acknowledge/clear any pending GPIO interrupt
   */

  CODE int  (*attach)(FAR const struct tca8418_config_s *config, xcpt_t isr,
                      FAR void *arg);
  CODE void (*enable)(FAR const struct tca8418_config_s *config,
                      bool enable);
  CODE void (*clear)(FAR const struct tca8418_config_s *config);
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
 * Name: tca8418_register
 *
 * Description:
 *   Configure the TCA8418 keypad scanner and register it as a keyboard.
 *
 * Input Parameters:
 *   i2c     - An I2C driver instance for the bus the device is on.
 *   config  - Persistent board configuration data, see above.  The driver
 *             keeps this reference, so it must not be on the stack.
 *   devname - The device path, typically "/dev/kbd0".
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int tca8418_register(FAR struct i2c_master_s *i2c,
                     FAR const struct tca8418_config_s *config,
                     FAR const char *devname);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* CONFIG_INPUT_TCA8418 */
#endif /* __INCLUDE_NUTTX_INPUT_TCA8418_H */
