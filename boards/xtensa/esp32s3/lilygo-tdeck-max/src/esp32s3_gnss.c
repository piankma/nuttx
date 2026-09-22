/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_gnss.c
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
 * The u-blox MIA-M10Q GNSS receiver talks NMEA on UART1 at 38400 baud.  This
 * is the lower half of NuttX's GNSS upper half, which parses the NMEA into
 * the uORB topics sensor_gnss and sensor_gnss_satellite and passes the raw
 * sentences through /dev/ttyGNSS0.
 *
 * The receiver is only powered while something uses it: the first user
 * switches the GPS rail on and starts a thread that feeds the UART's data
 * to the upper half.  A few seconds after the last user has gone, the
 * thread stops and the rail is switched off; the delay spares the receiver
 * a cold start when a program closes and reopens it straight away, as
 * uorb_listener does.  The rail holds the chip out of light sleep while it
 * is on, since the UART stops in light sleep.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <debug.h>

#include <nuttx/fs/fs.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sensors/gnss.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_SENSORS_GNSS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SCHED_LPWORK
#  error "The GNSS glue needs the low priority work queue (SCHED_LPWORK)"
#endif

#define GNSS_UART_PATH       "/dev/ttyS0"   /* UART1, the only UART here */
#define GNSS_THREAD_PRIORITY 110
#define GNSS_THREAD_STACK    3072
#define GNSS_POLL_MS         500            /* How soon the thread sees a stop */
#define GNSS_INTERVAL_US     1000000        /* The receiver's default 1 Hz */
#define GNSS_OFF_DELAY       SEC2TICK(5)    /* Power stays on after the last user */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tdeckmax_gnss_s
{
  struct gnss_lowerhalf_s lower;            /* Must be first */
  mutex_t lock;                             /* Serialises the UART's use */
  mutex_t statelock;                        /* Serialises power changes */
  sem_t exited;                             /* Posted as the thread ends */
  struct work_s offwork;                    /* Powers off after the delay */
  volatile bool running;                    /* Cleared to stop the thread */
  bool active;                              /* Someone uses the receiver */
  int fd;                                   /* The UART, while running */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int     tdeckmax_gnss_activate(FAR struct gnss_lowerhalf_s *lower,
                                      FAR struct file *filep, bool enable);
static int     tdeckmax_gnss_set_interval(FAR struct gnss_lowerhalf_s *lower,
                                          FAR struct file *filep,
                                          FAR uint32_t *period_us);
static ssize_t tdeckmax_gnss_inject(FAR struct gnss_lowerhalf_s *lower,
                                    FAR struct file *filep,
                                    FAR const void *buffer, size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gnss_ops_s g_gnss_ops =
{
  .activate     = tdeckmax_gnss_activate,
  .set_interval = tdeckmax_gnss_set_interval,
  .inject_data  = tdeckmax_gnss_inject,
};

static struct tdeckmax_gnss_s g_gnss =
{
  .lower =
    {
      .ops = &g_gnss_ops,
    },
  .lock      = NXMUTEX_INITIALIZER,
  .statelock = NXMUTEX_INITIALIZER,
  .exited    = SEM_INITIALIZER(0),
  .fd     = -1,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_gnss_thread
 *
 * Description:
 *   Feed the receiver's output to the upper half until told to stop.
 *
 ****************************************************************************/

static int tdeckmax_gnss_thread(int argc, FAR char *argv[])
{
  FAR struct tdeckmax_gnss_s *priv = &g_gnss;
  struct pollfd fds;
  char buffer[128];
  ssize_t nread;
  int fd;

  fd = nx_open(GNSS_UART_PATH, O_RDWR);
  if (fd < 0)
    {
      snerr("ERROR: Failed to open %s: %d\n", GNSS_UART_PATH, fd);
      goto out;
    }

  nxmutex_lock(&priv->lock);
  priv->fd = fd;
  nxmutex_unlock(&priv->lock);

  while (priv->running)
    {
      fds.fd      = fd;
      fds.events  = POLLIN;
      fds.revents = 0;

      if (poll(&fds, 1, GNSS_POLL_MS) <= 0)
        {
          continue;
        }

      nread = nx_read(fd, buffer, sizeof(buffer));
      if (nread > 0)
        {
          priv->lower.push_data(priv->lower.priv, buffer, nread, true);
        }
    }

  nxmutex_lock(&priv->lock);
  priv->fd = -1;
  nxmutex_unlock(&priv->lock);
  nx_close(fd);

out:
  nxsem_post(&priv->exited);
  return 0;
}

/****************************************************************************
 * Name: tdeckmax_gnss_off
 *
 * Description:
 *   Work queue job, some time after the last user has gone: stop the
 *   thread and switch the receiver off, unless it is in use again.
 *
 ****************************************************************************/

static void tdeckmax_gnss_off(FAR void *arg)
{
  FAR struct tdeckmax_gnss_s *priv = arg;

  nxmutex_lock(&priv->statelock);
  if (!priv->active && priv->running)
    {
      priv->running = false;
      nxsem_wait_uninterruptible(&priv->exited);
      tdeckmax_xl9555_rail(XL9555_PIN_GPS_EN, false);
    }

  nxmutex_unlock(&priv->statelock);
}

/****************************************************************************
 * Name: tdeckmax_gnss_activate
 ****************************************************************************/

static int tdeckmax_gnss_activate(FAR struct gnss_lowerhalf_s *lower,
                                  FAR struct file *filep, bool enable)
{
  FAR struct tdeckmax_gnss_s *priv = (FAR struct tdeckmax_gnss_s *)lower;
  int ret = OK;

  nxmutex_lock(&priv->statelock);

  priv->active = enable;
  if (!enable)
    {
      work_queue(LPWORK, &priv->offwork, tdeckmax_gnss_off, priv,
                 GNSS_OFF_DELAY);
    }
  else if (!priv->running)
    {
      ret = tdeckmax_xl9555_rail(XL9555_PIN_GPS_EN, true);
      if (ret >= 0)
        {
          priv->running = true;
          ret = kthread_create("gnss", GNSS_THREAD_PRIORITY,
                               GNSS_THREAD_STACK, tdeckmax_gnss_thread,
                               NULL);
          if (ret < 0)
            {
              priv->running = false;
              tdeckmax_xl9555_rail(XL9555_PIN_GPS_EN, false);
            }
        }
    }

  nxmutex_unlock(&priv->statelock);
  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: tdeckmax_gnss_set_interval
 ****************************************************************************/

static int tdeckmax_gnss_set_interval(FAR struct gnss_lowerhalf_s *lower,
                                      FAR struct file *filep,
                                      FAR uint32_t *period_us)
{
  /* The receiver reports once a second; changing that takes UBX
   * configuration, which can be sent with inject_data (a write to
   * /dev/ttyGNSS0).
   */

  *period_us = GNSS_INTERVAL_US;
  return OK;
}

/****************************************************************************
 * Name: tdeckmax_gnss_inject
 ****************************************************************************/

static ssize_t tdeckmax_gnss_inject(FAR struct gnss_lowerhalf_s *lower,
                                    FAR struct file *filep,
                                    FAR const void *buffer, size_t buflen)
{
  FAR struct tdeckmax_gnss_s *priv = (FAR struct tdeckmax_gnss_s *)lower;
  ssize_t ret = -EAGAIN;

  /* Commands for the receiver (NMEA or UBX) go straight to its UART */

  nxmutex_lock(&priv->lock);
  if (priv->fd >= 0)
    {
      ret = nx_write(priv->fd, buffer, buflen);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_gnss_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_gnss_initialize(void)
{
  uint32_t nbuffer[SENSOR_GNSS_IDX_GNSS_MAX] =
    {
      1, 1, 1, 1, 1
    };

  return gnss_register(&g_gnss.lower, 0, nbuffer, SENSOR_GNSS_IDX_GNSS_MAX);
}

#endif /* CONFIG_SENSORS_GNSS */
