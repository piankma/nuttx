/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_tca8418.c
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
 * The T-Deck Max carries a BlackBerry style thumb keyboard wired as a 4x10
 * matrix into a TCA8418 scanner on the shared I2C bus.  The scanner's
 * interrupt is on GPIO15 and its reset is one of the XL9555 expander lines,
 * released by the board's expander setup before this runs.
 *
 * The matrix is wired with its columns in the opposite order to the way the
 * keys are laid out, which is what "colreverse" below is for.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/irq.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/tca8418.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "esp32s3_i2c.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_INPUT_TCA8418

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TDECKMAX_KBD_ROWS   4
#define TDECKMAX_KBD_COLS   10

/* The keys that are not letters.  Enter and backspace are reported as
 * special keys rather than as characters, because what they should produce
 * depends on who is listening: a terminal wants "\n" for enter and DEL for
 * backspace (the NxTerm PTY bridge translates them so), while a toolkit
 * like LVGL has key codes of its own.  A raw "\r" from the keyboard, for
 * one, never ends a line in NSH.
 */

#define KBD_BS              TCA8418_SPEC(KEYCODE_BACKDEL)
#define KBD_CR              TCA8418_SPEC(KEYCODE_ENTER)

/* The keyboard is a BlackBerry Q20's.  Its Alt key (left, next to Z) is
 * held for the characters printed on the keys (the driver's symbol
 * layer), and with it enter, backspace and space become keys of their
 * own, which a user interface gives meanings to: menu (options), cancel
 * (back) and page up.  Alt + shift toggles caps lock.  Its Sym key (right
 * of space) is Find.  Shift + space is page up too.
 */

#define KBD_MENU            TCA8418_SPEC(KEYCODE_MENU)
#define KBD_CANCEL          TCA8418_SPEC(KEYCODE_CANCEL)
#define KBD_PGUP            TCA8418_SPEC(KEYCODE_PAGEUP)
#define KBD_FIND            TCA8418_SPEC(KEYCODE_FIND)
#define KBD_ALT             TCA8418_SYM     /* Held: the printed layer */
#define KBD_CAPS            TCA8418_ALT     /* Toggles caps lock */
#define KBD_SP              ' '
#define KBD__                TCA8418_NONE

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  tdeckmax_kbd_attach(FAR const struct tca8418_config_s *config,
                                xcpt_t isr, FAR void *arg);
static void tdeckmax_kbd_enable(FAR const struct tca8418_config_s *config,
                                bool enable);
static void tdeckmax_kbd_clear(FAR const struct tca8418_config_s *config);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The keyboard as it is printed on the keys, left to right and top to
 * bottom.  The bottom row has only five keys: shift, 0, space, sym and a
 * second shift.
 */

static const uint16_t g_kbd_base[TDECKMAX_KBD_ROWS * TDECKMAX_KBD_COLS] =
{
  'q',          'w',   'e',   'r',   't',   'y',   'u',   'i',   'o', 'p',
  'a',          's',   'd',   'f',   'g',   'h',   'j',   'k',   'l', KBD_BS,
  KBD_ALT,      'z',   'x',   'c',   'v',   'b',   'n',   'm',   '$', KBD_CR,
  KBD__, KBD__, KBD__, KBD__, KBD__,
  TCA8418_SHIFT, '0', KBD_SP, KBD_FIND, TCA8418_SHIFT
};

/* Held shift, or caps lock */

static const uint16_t g_kbd_shift[TDECKMAX_KBD_ROWS * TDECKMAX_KBD_COLS] =
{
  'Q',          'W',   'E',   'R',   'T',   'Y',   'U',   'I',   'O', 'P',
  'A',          'S',   'D',   'F',   'G',   'H',   'J',   'K',   'L', KBD_BS,
  KBD_ALT,      'Z',   'X',   'C',   'V',   'B',   'N',   'M',   '$', KBD_CR,
  KBD__, KBD__, KBD__, KBD__, KBD__,
  TCA8418_SHIFT, '0', KBD_PGUP, KBD_FIND, TCA8418_SHIFT
};

/* Held Alt: what is printed on the keys.  Anything left empty here falls
 * back to the base layer, so the digit and the modifiers keep working.
 */

static const uint16_t g_kbd_sym[TDECKMAX_KBD_ROWS * TDECKMAX_KBD_COLS] =
{
  '#',          '1',   '2',   '3',   '(',   ')',   '_',   '-',   '+', '@',
  '*',          '4',   '5',   '6',   '/',   ':',   ';',  '\'',  '"',
  KBD_CANCEL,
  KBD_ALT,      '7',   '8',   '9',   '?',   '!',   ',',   '.', KBD__,
  KBD_MENU,
  KBD__, KBD__, KBD__, KBD__, KBD__,
  KBD_CAPS, KBD__, KBD_PGUP, KBD_FIND, KBD_CAPS
};

static const struct tca8418_config_s g_tca8418_config =
{
  .frequency  = CONFIG_LILYGO_TDECK_MAX_TCA8418_FREQUENCY,
  .address    = BOARD_I2C_ADDR_TCA8418,
  .rows       = TDECKMAX_KBD_ROWS,
  .cols       = TDECKMAX_KBD_COLS,
  .colreverse = true,
  .base       = g_kbd_base,
  .shift      = g_kbd_shift,
  .sym        = g_kbd_sym,
  .attach     = tdeckmax_kbd_attach,
  .enable     = tdeckmax_kbd_enable,
  .clear      = tdeckmax_kbd_clear,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_kbd_attach
 ****************************************************************************/

static int tdeckmax_kbd_attach(FAR const struct tca8418_config_s *config,
                               xcpt_t isr, FAR void *arg)
{
  int ret;

  ret = esp_gpio_irq(BOARD_KEYBOARD_INT, isr, arg);
  if (ret < 0)
    {
      ierr("ERROR: Failed to attach GPIO%d: %d\n", BOARD_KEYBOARD_INT, ret);
      return ret;
    }

  /* esp_gpio_irq() enables the interrupt as a side effect; the driver asks
   * for it explicitly once it is ready.
   */

  esp_gpioirqdisable(BOARD_KEYBOARD_INT);
  return OK;
}

/****************************************************************************
 * Name: tdeckmax_kbd_enable
 ****************************************************************************/

static void tdeckmax_kbd_enable(FAR const struct tca8418_config_s *config,
                                bool enable)
{
  if (enable)
    {
      esp_gpioirqenable(BOARD_KEYBOARD_INT);
    }
  else
    {
      esp_gpioirqdisable(BOARD_KEYBOARD_INT);
    }
}

/****************************************************************************
 * Name: tdeckmax_kbd_clear
 *
 * Description:
 *   Nothing to do: the interrupt is level triggered, and the GPIO
 *   peripheral clears its own pending status when it is dispatched.
 *
 ****************************************************************************/

static void tdeckmax_kbd_clear(FAR const struct tca8418_config_s *config)
{
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_keyboard_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_keyboard_initialize(void)
{
  FAR struct i2c_master_s *i2c;

  /* The scanner pulls its interrupt line down and holds it there until the
   * event FIFO is read.  The interrupt is taken on the low level rather
   * than the falling edge: light sleep only wakes on levels, and an edge
   * that arrives while the chip sleeps is lost, which would leave the line
   * low with nothing ever reading the FIFO.  The driver masks the
   * interrupt until it has drained the FIFO.
   */

  esp_configgpio(BOARD_KEYBOARD_INT, INPUT | PULLUP | ONLOW);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  esp32s3_sleep_wake_on_gpio(BOARD_KEYBOARD_INT, false);
#endif

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  if (i2c == NULL)
    {
      ierr("ERROR: Failed to initialize I2C bus %d\n", TDECKMAX_I2C_PORT);
      return -ENODEV;
    }

  return tca8418_register(i2c, &g_tca8418_config, "/dev/kbd0");
}

#endif /* CONFIG_INPUT_TCA8418 */
