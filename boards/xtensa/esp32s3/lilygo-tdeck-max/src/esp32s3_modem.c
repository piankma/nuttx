/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_modem.c
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
 * The modem's ring indicator and DTR, for sleeping with the modem on.
 *
 * The UART stops in light sleep, so what the modem says while the chip
 * sleeps is lost.  RI (GPIO7, active low) tells when it has something to
 * say: an incoming call or SMS, or another URC (AT+CFGRI).  It wakes the
 * chip from light sleep, holds it awake for a while so that the words can
 * come in, and is counted for modemd, which then asks the modem what it
 * was.  DTR (GPIO8) lets the modem sleep too, with AT+CSCLK=1: high, it
 * may; low, it stays awake for commands.
 *
 *   /dev/modem_sleep   read: RI pulses since the last read (uint32_t),
 *                      then, if there is room, RI's level now (uint32_t);
 *                      poll: POLLIN after a pulse; write "1" or "0": DTR
 *   /dev/awake         the chip stays awake while it is open
 *
 * Both lines go through the modem's level shifter, which the modem's own
 * 1.8 V supplies: while the modem is off, RI reads low.  A modem that is
 * off is not listened to (the rail's handler calls
 * tdeckmax_modem_listen()), or it would keep the chip awake.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <debug.h>

#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "espressif/esp_gpio.h"
#include "esp32s3_sleep.h"

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_LILYGO_TDECK_MAX_MODEM_SLEEP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MODEM_RI_AWAKE    MSEC2TICK(2000)  /* Awake after a pulse */
#define MODEM_RI_POLL     MSEC2TICK(30)    /* While RI is low */

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     modem_open(FAR struct file *filep);
static int     modem_close(FAR struct file *filep);
static ssize_t modem_read(FAR struct file *filep, FAR char *buffer,
                          size_t len);
static ssize_t modem_write(FAR struct file *filep, FAR const char *buffer,
                           size_t len);
static int     modem_poll(FAR struct file *filep, FAR struct pollfd *fds,
                          bool setup);
static int     awake_open(FAR struct file *filep);
static int     awake_close(FAR struct file *filep);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_modem_fops =
{
  .open  = modem_open,
  .close = modem_close,
  .read  = modem_read,
  .write = modem_write,
  .poll  = modem_poll,
};

static const struct file_operations g_awake_fops =
{
  .open  = awake_open,
  .close = awake_close,
};

static mutex_t g_modem_lock = NXMUTEX_INITIALIZER;
static FAR struct pollfd *g_modem_fds[2];
static volatile uint32_t g_ri_count;
static uint32_t g_ri_read;
static bool g_listening;
static bool g_ri_held;                /* Holding the chip after a pulse */
static struct work_s g_ri_work;
static struct work_s g_ri_hold_work;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void modem_notify(void)
{
  poll_notify(g_modem_fds, 2, POLLIN);
}

/* Two seconds after a pulse: let the chip sleep again */

static void modem_ri_unhold(FAR void *arg)
{
  irqstate_t flags = enter_critical_section();

  if (g_ri_held)
    {
      g_ri_held = false;
      leave_critical_section(flags);
      esp32s3_sleep_release();
      return;
    }

  leave_critical_section(flags);
}

/* RI went low: wait for it to go back up (the level interrupt would fire
 * all the while), then listen again
 */

static void modem_ri_worker(FAR void *arg)
{
  if (!esp_gpioread(BOARD_MODEM_RI) && g_listening)
    {
      work_queue(LPWORK, &g_ri_work, modem_ri_worker, NULL, MODEM_RI_POLL);
      return;
    }

  if (g_listening)
    {
      esp_gpioirqenable(BOARD_MODEM_RI);
    }
}

static int modem_ri_interrupt(int irq, FAR void *context, FAR void *arg)
{
  esp_gpioirqdisable(BOARD_MODEM_RI);
  g_ri_count++;

  if (!g_ri_held)
    {
      g_ri_held = true;
      esp32s3_sleep_hold();
    }

  work_queue(LPWORK, &g_ri_hold_work, modem_ri_unhold, NULL,
             MODEM_RI_AWAKE);
  work_queue(LPWORK, &g_ri_work, modem_ri_worker, NULL, MODEM_RI_POLL);
  modem_notify();
  return OK;
}

static int modem_open(FAR struct file *filep)
{
  return OK;
}

static int modem_close(FAR struct file *filep)
{
  return OK;
}

static ssize_t modem_read(FAR struct file *filep, FAR char *buffer,
                          size_t len)
{
  uint32_t n;

  if (len < sizeof(n))
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_modem_lock);
  n = g_ri_count - g_ri_read;
  g_ri_read = g_ri_count;
  nxmutex_unlock(&g_modem_lock);

  memcpy(buffer, &n, sizeof(n));
  if (len < 2 * sizeof(n))
    {
      return sizeof(n);
    }

  n = esp_gpioread(BOARD_MODEM_RI);
  memcpy(buffer + sizeof(n), &n, sizeof(n));
  return 2 * sizeof(n);
}

static ssize_t modem_write(FAR struct file *filep, FAR const char *buffer,
                           size_t len)
{
  if (len < 1 || (buffer[0] != '0' && buffer[0] != '1'))
    {
      return -EINVAL;
    }

  esp_gpiowrite(BOARD_MODEM_DTR, buffer[0] == '1');
  return len;
}

static int modem_poll(FAR struct file *filep, FAR struct pollfd *fds,
                      bool setup)
{
  int ret = OK;
  int i;

  nxmutex_lock(&g_modem_lock);
  if (setup)
    {
      for (i = 0; i < 2 && g_modem_fds[i] != NULL; i++);
      if (i == 2)
        {
          ret = -EBUSY;
        }
      else
        {
          g_modem_fds[i] = fds;
          fds->priv = &g_modem_fds[i];
          if (g_ri_count != g_ri_read)
            {
              poll_notify(&fds, 1, POLLIN);
            }
        }
    }
  else if (fds->priv != NULL)
    {
      *(FAR struct pollfd **)fds->priv = NULL;
      fds->priv = NULL;
    }

  nxmutex_unlock(&g_modem_lock);
  return ret;
}

static int awake_open(FAR struct file *filep)
{
  esp32s3_sleep_hold();
  return OK;
}

static int awake_close(FAR struct file *filep)
{
  esp32s3_sleep_release();
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_modem_listen
 *
 * Description:
 *   Listen to RI while the modem is on (its level shifter is powered), and
 *   not while it is off (RI would read low for good).
 *
 ****************************************************************************/

void tdeckmax_modem_listen(bool on)
{
  g_listening = on;
  if (on)
    {
      esp32s3_sleep_wake_on_gpio(BOARD_MODEM_RI, false);
      esp_gpioirqenable(BOARD_MODEM_RI);
    }
  else
    {
      esp_gpioirqdisable(BOARD_MODEM_RI);
      esp32s3_sleep_wake_on_gpio_off(BOARD_MODEM_RI);
    }
}

/****************************************************************************
 * Name: tdeckmax_modem_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_modem_initialize(void)
{
  int ret;

  esp_configgpio(BOARD_MODEM_DTR, OUTPUT);
  esp_gpiowrite(BOARD_MODEM_DTR, false);    /* The modem stays awake */
  esp_configgpio(BOARD_MODEM_RI, INPUT | ONLOW);

  ret = esp_gpio_irq(BOARD_MODEM_RI, modem_ri_interrupt, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: modem RI on GPIO%d: %d\n", BOARD_MODEM_RI,
             ret);
      return ret;
    }

  esp_gpioirqdisable(BOARD_MODEM_RI);       /* Until the modem is on */

  ret = register_driver("/dev/modem_sleep", &g_modem_fops, 0666, NULL);
  if (ret == OK)
    {
      ret = register_driver("/dev/awake", &g_awake_fops, 0666, NULL);
    }

  return ret;
}

#endif /* CONFIG_LILYGO_TDECK_MAX_MODEM_SLEEP */
