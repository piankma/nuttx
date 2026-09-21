/****************************************************************************
 * drivers/input/tca8418.c
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
 * Driver for the TI TCA8418 I2C keypad scanner.
 *
 * The controller scans the matrix itself and pushes press and release
 * events into a 10 deep FIFO, raising its interrupt line while anything is
 * waiting.  So there is no polling here: the interrupt schedules a worker,
 * the worker drains the FIFO, translates each event through the board's
 * keymap and hands it to the keyboard upper half.
 *
 * Modifier keys never reach the upper half.  They select which of the
 * board's keymap layers the following keys are looked up in, which is how
 * a 4x10 keyboard covers letters, capitals and punctuation.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/input/tca8418.h>
#include <nuttx/wqueue.h>

#ifdef CONFIG_INPUT_TCA8418

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Registers */

#define TCA8418_CFG             0x01  /* Configuration */
#define TCA8418_INT_STAT        0x02  /* Interrupt status */
#define TCA8418_KEY_LCK_EC      0x03  /* Key lock and event counter */
#define TCA8418_KEY_EVENT_A     0x04  /* Key event FIFO, oldest entry */
#define TCA8418_GPIO_INT_STAT_1 0x11  /* GPIO interrupt status */
#define TCA8418_GPIO_INT_EN_1   0x1a  /* GPIO interrupt enable */
#define TCA8418_KP_GPIO_1       0x1d  /* Keypad or GPIO select */
#define TCA8418_GPI_EM_1        0x20  /* GPI event mode */
#define TCA8418_GPIO_DIR_1      0x23  /* GPIO data direction */
#define TCA8418_GPIO_INT_LVL_1  0x26  /* GPIO edge/level select */

/* CFG bits */

#define TCA8418_CFG_KE_IEN      0x01  /* Key event interrupt enable */
#define TCA8418_CFG_GPI_IEN     0x02  /* GPI interrupt enable */

/* INT_STAT bits.  Both are write-1-to-clear */

#define TCA8418_INT_STAT_K      0x01  /* Key event interrupt */
#define TCA8418_INT_STAT_GPI    0x02  /* GPI interrupt */

/* KEY_LCK_EC: the low nibble counts the events waiting in the FIFO */

#define TCA8418_EVENT_COUNT     0x0f

/* A key event: bit 7 is set for a press and clear for a release, and the
 * low seven bits are the key number.
 */

#define TCA8418_EVENT_PRESS     0x80
#define TCA8418_EVENT_KEY       0x7f

/* The controller numbers keys as row * 10 + column + 1 whatever the size of
 * the configured matrix, so the stride is a property of the chip and not of
 * the board's "cols".
 */

#define TCA8418_KEY_STRIDE      10

/* The FIFO is 10 entries deep.  Draining more than that in one pass means
 * something is wrong, and bailing out is better than spinning forever on a
 * bus that keeps answering.
 */

#define TCA8418_FIFO_DEPTH      10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tca8418_dev_s
{
  struct keyboard_lowerhalf_s lower;          /* Upper half interface */
  FAR struct i2c_master_s *i2c;               /* I2C bus */
  FAR const struct tca8418_config_s *config;  /* Board configuration */
  struct work_s work;                         /* Deferred FIFO read */

  uint8_t shift;                              /* Shift keys held down */
  uint8_t sym;                                /* Symbol keys held down */
  bool    caps;                               /* Caps lock, toggled by ALT */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  tca8418_getreg(FAR struct tca8418_dev_s *priv, uint8_t reg,
                           FAR uint8_t *value);
static int  tca8418_putreg(FAR struct tca8418_dev_s *priv, uint8_t reg,
                           uint8_t value);
static int  tca8418_configure(FAR struct tca8418_dev_s *priv);
static void tca8418_report(FAR struct tca8418_dev_s *priv, uint16_t key,
                           bool press);
static void tca8418_event(FAR struct tca8418_dev_s *priv, uint8_t event);
static void tca8418_worker(FAR void *arg);
static int  tca8418_interrupt(int irq, FAR void *context, FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tca8418_getreg
 ****************************************************************************/

static int tca8418_getreg(FAR struct tca8418_dev_s *priv, uint8_t reg,
                          FAR uint8_t *value)
{
  struct i2c_msg_s msg[2];
  int ret;

  msg[0].frequency = priv->config->frequency;
  msg[0].addr      = priv->config->address;
  msg[0].flags     = 0;
  msg[0].buffer    = &reg;
  msg[0].length    = 1;

  msg[1].frequency = priv->config->frequency;
  msg[1].addr      = priv->config->address;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = value;
  msg[1].length    = 1;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);
  if (ret < 0)
    {
      ierr("ERROR: Failed to read register 0x%02x: %d\n", reg, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: tca8418_putreg
 ****************************************************************************/

static int tca8418_putreg(FAR struct tca8418_dev_s *priv, uint8_t reg,
                          uint8_t value)
{
  struct i2c_msg_s msg;
  uint8_t buffer[2];
  int ret;

  buffer[0] = reg;
  buffer[1] = value;

  msg.frequency = priv->config->frequency;
  msg.addr      = priv->config->address;
  msg.flags     = 0;
  msg.buffer    = buffer;
  msg.length    = 2;

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);
  if (ret < 0)
    {
      ierr("ERROR: Failed to write register 0x%02x: %d\n", reg, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: tca8418_configure
 *
 * Description:
 *   Put the controller in keypad scanning mode for the board's matrix and
 *   enable the key event interrupt.  Any events left over from before are
 *   dropped, so the first key the user presses is the first one reported.
 *
 ****************************************************************************/

static int tca8418_configure(FAR struct tca8418_dev_s *priv)
{
  FAR const struct tca8418_config_s *config = priv->config;
  uint8_t value;
  int count;
  int ret;

  /* Every pin is an input. */

  ret = tca8418_putreg(priv, TCA8418_GPIO_DIR_1, 0x00);
  if (ret < 0)
    {
      return ret;
    }

  tca8418_putreg(priv, TCA8418_GPIO_DIR_1 + 1, 0x00);
  tca8418_putreg(priv, TCA8418_GPIO_DIR_1 + 2, 0x00);

  /* Nothing outside the matrix may raise an event.  The pins the matrix
   * uses are handed to the key scanner below, and the scanner does not go
   * through this path, so these registers only govern the pins that are
   * left over.  Those are typically unconnected and floating, and letting
   * a floating pin push entries into the same event FIFO as the keyboard
   * would produce phantom keys and a stream of interrupts with no key
   * behind them.
   */

  tca8418_putreg(priv, TCA8418_GPI_EM_1, 0x00);
  tca8418_putreg(priv, TCA8418_GPI_EM_1 + 1, 0x00);
  tca8418_putreg(priv, TCA8418_GPI_EM_1 + 2, 0x00);

  tca8418_putreg(priv, TCA8418_GPIO_INT_LVL_1, 0x00);
  tca8418_putreg(priv, TCA8418_GPIO_INT_LVL_1 + 1, 0x00);
  tca8418_putreg(priv, TCA8418_GPIO_INT_LVL_1 + 2, 0x00);

  tca8418_putreg(priv, TCA8418_GPIO_INT_EN_1, 0x00);
  tca8418_putreg(priv, TCA8418_GPIO_INT_EN_1 + 1, 0x00);
  tca8418_putreg(priv, TCA8418_GPIO_INT_EN_1 + 2, 0x00);

  /* Hand the matrix pins over to the key scanner.  KP_GPIO_1 covers rows
   * 0 to 7, KP_GPIO_2 columns 0 to 7 and KP_GPIO_3 columns 8 and 9.
   */

  tca8418_putreg(priv, TCA8418_KP_GPIO_1,
                 (uint8_t)((1 << config->rows) - 1));
  tca8418_putreg(priv, TCA8418_KP_GPIO_1 + 1,
                 config->cols >= 8 ?
                 0xff : (uint8_t)((1 << config->cols) - 1));
  tca8418_putreg(priv, TCA8418_KP_GPIO_1 + 2,
                 config->cols > 8 ?
                 (uint8_t)((1 << (config->cols - 8)) - 1) : 0x00);

  /* Drop anything the controller collected before we were listening */

  for (count = 0; count < TCA8418_FIFO_DEPTH; count++)
    {
      if (tca8418_getreg(priv, TCA8418_KEY_EVENT_A, &value) < 0 ||
          value == 0)
        {
          break;
        }
    }

  tca8418_getreg(priv, TCA8418_GPIO_INT_STAT_1, &value);
  tca8418_getreg(priv, TCA8418_GPIO_INT_STAT_1 + 1, &value);
  tca8418_getreg(priv, TCA8418_GPIO_INT_STAT_1 + 2, &value);
  tca8418_putreg(priv, TCA8418_INT_STAT,
                 TCA8418_INT_STAT_K | TCA8418_INT_STAT_GPI);

  /* Finally let the controller interrupt us on a key event */

  ret = tca8418_getreg(priv, TCA8418_CFG, &value);
  if (ret < 0)
    {
      return ret;
    }

  return tca8418_putreg(priv, TCA8418_CFG, value | TCA8418_CFG_KE_IEN);
}

/****************************************************************************
 * Name: tca8418_report
 *
 * Description:
 *   Hand one translated key to the keyboard upper half.  Characters and
 *   special keys share a value range, so the type is what tells them apart.
 *
 ****************************************************************************/

static void tca8418_report(FAR struct tca8418_dev_s *priv, uint16_t key,
                           bool press)
{
  if (key >= TCA8418_SPEC(0))
    {
      keyboard_event(&priv->lower, key - TCA8418_SPEC(0),
                     press ? KEYBOARD_SPECPRESS : KEYBOARD_SPECREL);
    }
  else
    {
      keyboard_event(&priv->lower, key,
                     press ? KEYBOARD_PRESS : KEYBOARD_RELEASE);
    }
}

/****************************************************************************
 * Name: tca8418_event
 *
 * Description:
 *   Translate one raw FIFO entry and either act on it, if it is a modifier,
 *   or report it.
 *
 ****************************************************************************/

static void tca8418_event(FAR struct tca8418_dev_s *priv, uint8_t event)
{
  FAR const struct tca8418_config_s *config = priv->config;
  FAR const uint16_t *keymap;
  bool press = (event & TCA8418_EVENT_PRESS) != 0;
  uint8_t code = (event & TCA8418_EVENT_KEY);
  uint16_t key;
  uint8_t row;
  uint8_t col;

  /* Key numbers are one based, and zero means the FIFO was empty */

  if (code == 0)
    {
      return;
    }

  code--;
  row = code / TCA8418_KEY_STRIDE;
  col = code % TCA8418_KEY_STRIDE;

  if (row >= config->rows || col >= config->cols)
    {
      iwarn("WARNING: Event 0x%02x is outside the %ux%u matrix\n",
            event, config->rows, config->cols);
      return;
    }

  if (config->colreverse)
    {
      col = (config->cols - 1) - col;
    }

  /* Pick the layer.  A modifier is looked up in the base layer so that it
   * keeps working while another modifier is held.
   */

  keymap = config->base;
  if (priv->sym > 0 && config->sym != NULL)
    {
      keymap = config->sym;
    }
  else if ((priv->shift > 0 || priv->caps) && config->shift != NULL)
    {
      keymap = config->shift;
    }

  key = keymap[row * config->cols + col];
  if (key == TCA8418_NONE)
    {
      key = config->base[row * config->cols + col];
    }

  iinfo("row %u col %u %s key 0x%04x\n",
        row, col, press ? "press" : "release", key);

  switch (key)
    {
      case TCA8418_NONE:
        break;

      case TCA8418_SHIFT:
        if (press)
          {
            priv->shift++;
          }
        else if (priv->shift > 0)
          {
            priv->shift--;
          }
        break;

      case TCA8418_SYM:
        if (press)
          {
            priv->sym++;
          }
        else if (priv->sym > 0)
          {
            priv->sym--;
          }
        break;

      case TCA8418_ALT:

        /* Caps lock toggles on the press and ignores the release */

        if (press)
          {
            priv->caps = !priv->caps;
          }
        break;

      default:
        tca8418_report(priv, key, press);
        break;
    }
}

/****************************************************************************
 * Name: tca8418_worker
 *
 * Description:
 *   Drain the controller's event FIFO.  Runs on the high priority work
 *   queue because it talks to a possibly slow I2C bus, which an interrupt
 *   handler must not do.
 *
 ****************************************************************************/

static void tca8418_worker(FAR void *arg)
{
  FAR struct tca8418_dev_s *priv = (FAR struct tca8418_dev_s *)arg;
  uint8_t value;
  int count;

  DEBUGASSERT(priv != NULL && priv->config != NULL);

  if (priv->config->clear != NULL)
    {
      priv->config->clear(priv->config);
    }

  /* Read events until the FIFO reports empty.  The count register is not
   * consulted for the loop bound: a key pressed while we are draining adds
   * to the FIFO, and reading until it returns zero picks that up too.
   */

  for (count = 0; count < TCA8418_FIFO_DEPTH; count++)
    {
      if (tca8418_getreg(priv, TCA8418_KEY_EVENT_A, &value) < 0)
        {
          break;
        }

      if (value == 0)
        {
          break;
        }

      tca8418_event(priv, value);
    }

  /* Acknowledge at the controller, which releases its interrupt line */

  tca8418_putreg(priv, TCA8418_INT_STAT,
                 TCA8418_INT_STAT_K | TCA8418_INT_STAT_GPI);

  /* A key pressed between the last FIFO read and the acknowledge above
   * would leave events waiting with the line already released, and the
   * edge that would have told us about it is gone.  Check once more, and
   * reschedule rather than lose the key.
   */

  if (tca8418_getreg(priv, TCA8418_KEY_LCK_EC, &value) >= 0 &&
      (value & TCA8418_EVENT_COUNT) != 0)
    {
      /* Come back through the work queue rather than looping here, and
       * after a tick rather than immediately: this runs at high priority,
       * and a controller that kept reporting events would otherwise be
       * able to starve everything else in the system.
       */

      work_queue(HPWORK, &priv->work, tca8418_worker, priv, 1);
      return;
    }

  if (priv->config->enable != NULL)
    {
      priv->config->enable(priv->config, true);
    }
}

/****************************************************************************
 * Name: tca8418_interrupt
 ****************************************************************************/

static int tca8418_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct tca8418_dev_s *priv = (FAR struct tca8418_dev_s *)arg;
  int ret;

  DEBUGASSERT(priv != NULL);

  /* The controller holds its interrupt line down until the events are read,
   * which cannot be done here, so mask the interrupt until the worker has
   * drained the FIFO.
   */

  if (priv->config->enable != NULL)
    {
      priv->config->enable(priv->config, false);
    }

  if (work_available(&priv->work))
    {
      ret = work_queue(HPWORK, &priv->work, tca8418_worker, priv, 0);
      if (ret < 0)
        {
          ierr("ERROR: Failed to queue work: %d\n", ret);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tca8418_register
 *
 * Description:
 *   See include/nuttx/input/tca8418.h
 *
 ****************************************************************************/

int tca8418_register(FAR struct i2c_master_s *i2c,
                     FAR const struct tca8418_config_s *config,
                     FAR const char *devname)
{
  FAR struct tca8418_dev_s *priv;
  int ret;

  DEBUGASSERT(i2c != NULL && config != NULL && devname != NULL);
  DEBUGASSERT(config->attach != NULL && config->enable != NULL);
  DEBUGASSERT(config->base != NULL);

  if (config->rows == 0 || config->rows > TCA8418_MAX_ROWS ||
      config->cols == 0 || config->cols > TCA8418_MAX_COLS)
    {
      ierr("ERROR: %ux%u is not a matrix this controller can scan\n",
           config->rows, config->cols);
      return -EINVAL;
    }

  priv = kmm_zalloc(sizeof(struct tca8418_dev_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->i2c    = i2c;
  priv->config = config;

  /* Keep the interrupt masked until the controller is configured */

  config->enable(config, false);

  ret = tca8418_configure(priv);
  if (ret < 0)
    {
      ierr("ERROR: Failed to configure the keypad: %d\n", ret);
      goto errout;
    }

  ret = config->attach(config, tca8418_interrupt, priv);
  if (ret < 0)
    {
      ierr("ERROR: Failed to attach the interrupt: %d\n", ret);
      goto errout;
    }

  ret = keyboard_register(&priv->lower, devname,
                          CONFIG_INPUT_TCA8418_BUFSIZE);
  if (ret < 0)
    {
      ierr("ERROR: Failed to register %s: %d\n", devname, ret);
      goto errout;
    }

  config->enable(config, true);

  iinfo("Registered %s: %ux%u matrix at address 0x%02x\n",
        devname, config->rows, config->cols, config->address);
  return OK;

errout:
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_INPUT_TCA8418 */
