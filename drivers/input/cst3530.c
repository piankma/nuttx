/****************************************************************************
 * drivers/input/cst3530.c
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
 * Driver for the Hynitron CST3530 capacitive touch controller.  Hynitron's
 * own driver handles it together with the CST66xx parts as one family
 * ("cst66xx"), and so should this one.
 *
 * Register addresses are 32 bits, sent big endian, and writing a register
 * address with no data is how the controller takes its commands.  Each
 * transfer is a write of the address, a stop, and a separate read.  The
 * controller's I2C interface can be in a low power state in which the first
 * transfer only wakes it, so the wake command is always sent twice.
 *
 * A report is read from 0xD0070000:
 *
 *   0, 1   checksum: 0x55 plus the sum of the record bytes, little endian
 *   2      report type, 0xff for touches and keys
 *   3      touch records (low nibble) and touch key records (high nibble)
 *   4...   the records, 5 bytes each, keys first
 *
 * Only the first record comes with the address; the rest are read by
 * carrying on with a plain read.  A record is
 *
 *   0      X bits 7-0
 *   1      Y bits 7-0
 *   2      pressure
 *   3      X bits 11-8 (low nibble), Y bits 11-8 (high nibble)
 *   4      touch id (low nibble), non-zero while pressed (high nibble)
 *
 * Writing 0xD00002AB acknowledges the report.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <nuttx/debug.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/sched.h>
#include <nuttx/wqueue.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/input/cst3530.h>

#ifdef CONFIG_INPUT_CST3530

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SCHED_HPWORK
#  error "The CST3530 driver needs the high priority work queue"
#endif

/* Commands, and the registers read */

#define CST3530_CMD_WAKE       0xd0000400  /* Leave low power I2C */
#define CST3530_CMD_NORMAL1    0xd0000000  /* These three: normal mode */
#define CST3530_CMD_NORMAL2    0xd0000c00
#define CST3530_CMD_NORMAL3    0xd0000100
#define CST3530_CMD_ACK        0xd00002ab  /* Report read */
#define CST3530_CMD_DEEPSLEEP  0xd00022ab

#define CST3530_REG_INFO       0xd0030000
#define CST3530_REG_REPORT     0xd0070000

/* The info block, valid in normal mode */

#define CST3530_INFO_SIZE      50
#define CST3530_INFO_MAGIC     0xca        /* Bytes 2 and 3 */
#define CST3530_INFO_TRIES     4

/* Reports */

#define CST3530_REPORT_TOUCH   0xff
#define CST3530_HEADER_SIZE    4
#define CST3530_RECORD_SIZE    5
#define CST3530_MAXRECORDS     5           /* Touches and keys together */
#define CST3530_CHECKSUM_SEED  0x55
#define CST3530_MAXID          16          /* Touch ids are a nibble */

/* Timing, from Hynitron's driver */

#define CST3530_RESET_US       8000        /* Reset pulse */
#define CST3530_BOOT_US        50000       /* Reset to ready */
#define CST3530_WAKE_US        1000        /* Between the two wake commands */
#define CST3530_RETRY_US       10000       /* Between info attempts */

/* Touch samples are buffered for this many reads */

#define CST3530_BUFFERS        8

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cst3530_dev_s
{
  struct touch_lowerhalf_s lower;  /* Must be first */

  FAR struct i2c_master_s *i2c;
  FAR const struct cst3530_config_s *config;
  struct i2c_config_s i2cconfig;

  mutex_t lock;                    /* Protects everything below */
  struct work_s work;              /* Reads a report */
  int nopen;                       /* Open files; awake while non-zero */
  uint16_t downmap;                /* Touch ids currently down */
  int16_t lastx[CST3530_MAXID];    /* Last position of each id */
  int16_t lasty[CST3530_MAXID];

  uint8_t sample[SIZEOF_TOUCH_SAMPLE_S(CST3530_MAXRECORDS + CST3530_MAXID)];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int cst3530_command(FAR struct cst3530_dev_s *priv, uint32_t cmd);
static int cst3530_read(FAR struct cst3530_dev_s *priv, uint32_t reg,
                        FAR uint8_t *buffer, int len);
static int cst3530_normal(FAR struct cst3530_dev_s *priv);
static void cst3530_sleep(FAR struct cst3530_dev_s *priv);
static void cst3530_reset(FAR struct cst3530_dev_s *priv);
static int cst3530_report(FAR struct cst3530_dev_s *priv,
                          FAR uint8_t *buffer);
static void cst3530_worker(FAR void *arg);
static int cst3530_interrupt(int irq, FAR void *context, FAR void *arg);

static int cst3530_open(FAR struct touch_lowerhalf_s *lower);
static int cst3530_close(FAR struct touch_lowerhalf_s *lower);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cst3530_command
 *
 * Description:
 *   Write a register address with no data, which is how the controller
 *   takes its commands.
 *
 ****************************************************************************/

static int cst3530_command(FAR struct cst3530_dev_s *priv, uint32_t cmd)
{
  uint8_t buffer[4];

  buffer[0] = cmd >> 24;
  buffer[1] = cmd >> 16;
  buffer[2] = cmd >> 8;
  buffer[3] = cmd;

  return i2c_write(priv->i2c, &priv->i2cconfig, buffer, sizeof(buffer));
}

/****************************************************************************
 * Name: cst3530_read
 ****************************************************************************/

static int cst3530_read(FAR struct cst3530_dev_s *priv, uint32_t reg,
                        FAR uint8_t *buffer, int len)
{
  int ret;

  ret = cst3530_command(priv, reg);
  if (ret >= 0)
    {
      ret = i2c_read(priv->i2c, &priv->i2cconfig, buffer, len);
    }

  return ret;
}

/****************************************************************************
 * Name: cst3530_normal
 *
 * Description:
 *   Wake the I2C interface and put the controller in normal reporting
 *   mode.
 *
 ****************************************************************************/

static int cst3530_normal(FAR struct cst3530_dev_s *priv)
{
  int ret;

  /* The first wake may not be acknowledged: it is what does the waking */

  cst3530_command(priv, CST3530_CMD_WAKE);
  nxsched_usleep(CST3530_WAKE_US);

  ret = cst3530_command(priv, CST3530_CMD_WAKE);
  if (ret >= 0)
    {
      ret = cst3530_command(priv, CST3530_CMD_NORMAL1);
    }

  if (ret >= 0)
    {
      ret = cst3530_command(priv, CST3530_CMD_NORMAL2);
    }

  if (ret >= 0)
    {
      ret = cst3530_command(priv, CST3530_CMD_NORMAL3);
    }

  return ret;
}

/****************************************************************************
 * Name: cst3530_sleep
 *
 * Description:
 *   Put the controller in deep sleep.  Only a reset wakes it.
 *
 ****************************************************************************/

static void cst3530_sleep(FAR struct cst3530_dev_s *priv)
{
  cst3530_command(priv, CST3530_CMD_WAKE);
  nxsched_usleep(CST3530_WAKE_US);
  cst3530_command(priv, CST3530_CMD_WAKE);
  cst3530_command(priv, CST3530_CMD_DEEPSLEEP);
}

/****************************************************************************
 * Name: cst3530_reset
 *
 * Description:
 *   Pulse the reset line and wait for the controller to come up.
 *
 ****************************************************************************/

static void cst3530_reset(FAR struct cst3530_dev_s *priv)
{
  priv->config->reset(priv->config, true);
  nxsched_usleep(CST3530_RESET_US);
  priv->config->reset(priv->config, false);
  nxsched_usleep(CST3530_BOOT_US);
}

/****************************************************************************
 * Name: cst3530_report
 *
 * Description:
 *   Read a whole report into buffer and check it.  Returns the number of
 *   records, or a negated errno value.
 *
 ****************************************************************************/

static int cst3530_report(FAR struct cst3530_dev_s *priv,
                          FAR uint8_t *buffer)
{
  uint16_t sum;
  int nrecords;
  int ret;
  int i;

  ret = cst3530_read(priv, CST3530_REG_REPORT, buffer,
                     CST3530_HEADER_SIZE + CST3530_RECORD_SIZE);
  if (ret < 0)
    {
      return ret;
    }

  nrecords = (buffer[3] & 0x0f) + (buffer[3] >> 4);
  if (nrecords > CST3530_MAXRECORDS)
    {
      return -EIO;
    }

  if (nrecords > 1)
    {
      ret = i2c_read(priv->i2c, &priv->i2cconfig,
                     &buffer[CST3530_HEADER_SIZE + CST3530_RECORD_SIZE],
                     (nrecords - 1) * CST3530_RECORD_SIZE);
      if (ret < 0)
        {
          return ret;
        }
    }

  sum = CST3530_CHECKSUM_SEED;
  for (i = 0; i < nrecords * CST3530_RECORD_SIZE; i++)
    {
      sum += buffer[CST3530_HEADER_SIZE + i];
    }

  if (sum != (buffer[0] | (buffer[1] << 8)))
    {
      return -EIO;
    }

  return nrecords;
}

/****************************************************************************
 * Name: cst3530_worker
 *
 * Description:
 *   Read a report, acknowledge it, and pass on what changed: new and moved
 *   touches, and a release for every touch that is no longer pressed.
 *
 ****************************************************************************/

static void cst3530_worker(FAR void *arg)
{
  FAR struct cst3530_dev_s *priv = arg;
  FAR struct touch_sample_s *sample =
    (FAR struct touch_sample_s *)priv->sample;
  FAR struct touch_point_s *point;
  uint8_t buffer[CST3530_HEADER_SIZE +
                 CST3530_MAXRECORDS * CST3530_RECORD_SIZE];
  uint64_t timestamp;
  uint16_t downmap = 0;
  uint16_t released;
  int npoints = 0;
  int nkeys;
  int ntouch;
  int ret;
  int i;

  nxmutex_lock(&priv->lock);

  if (priv->nopen == 0)
    {
      /* Closed while this was queued; the controller is asleep */

      nxmutex_unlock(&priv->lock);
      return;
    }

  /* One more try if the report was garbled, as Hynitron's driver does.
   * Either way the report is acknowledged, or no new one would come.
   */

  ret = cst3530_report(priv, buffer);
  if (ret < 0)
    {
      ret = cst3530_report(priv, buffer);
    }

  cst3530_command(priv, CST3530_CMD_ACK);

  if (ret < 0 || buffer[2] != CST3530_REPORT_TOUCH)
    {
      goto out;  /* Garbled, or a gesture or proximity report */
    }

  nkeys  = buffer[3] >> 4;
  ntouch = buffer[3] & 0x0f;

  if (nkeys > 0)
    {
      iinfo("Touch key %d, state %d\n",
            buffer[CST3530_HEADER_SIZE + 4] & 0x0f,
            buffer[CST3530_HEADER_SIZE + 4] >> 4);
    }

  timestamp = touch_get_time();
  memset(sample, 0, sizeof(priv->sample));

  for (i = 0; i < ntouch; i++)
    {
      FAR const uint8_t *raw =
        &buffer[CST3530_HEADER_SIZE + (nkeys + i) * CST3530_RECORD_SIZE];
      uint8_t id = raw[4] & 0x0f;

      if ((raw[4] >> 4) == 0)
        {
          continue;  /* Released; reported below */
        }

      priv->lastx[id] = raw[0] | ((int16_t)(raw[3] & 0x0f) << 8);
      priv->lasty[id] = raw[1] | ((int16_t)(raw[3] & 0xf0) << 4);

      point            = &sample->point[npoints++];
      point->id        = id;
      point->x         = priv->lastx[id];
      point->y         = priv->lasty[id];
      point->pressure  = raw[2];
      point->timestamp = timestamp;
      point->flags     = ((priv->downmap & (1 << id)) != 0 ? TOUCH_MOVE :
                                                            TOUCH_DOWN) |
                         TOUCH_ID_VALID | TOUCH_POS_VALID |
                         TOUCH_PRESSURE_VALID;

      downmap |= 1 << id;
    }

  /* Whatever was down and is not any more has been released, whether the
   * report said so or simply left it out.
   */

  released = priv->downmap & ~downmap;
  for (i = 0; released != 0 && i < CST3530_MAXID; i++)
    {
      if ((released & (1 << i)) != 0)
        {
          released &= ~(1 << i);

          point            = &sample->point[npoints++];
          point->id        = i;
          point->x         = priv->lastx[i];
          point->y         = priv->lasty[i];
          point->timestamp = timestamp;
          point->flags     = TOUCH_UP | TOUCH_ID_VALID | TOUCH_POS_VALID;
        }
    }

  priv->downmap = downmap;

  if (npoints > 0)
    {
      sample->npoints = npoints;
      touch_event(priv->lower.priv, sample);
    }

out:
  nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: cst3530_interrupt
 ****************************************************************************/

static int cst3530_interrupt(int irq, FAR void *context, FAR void *arg)
{
  FAR struct cst3530_dev_s *priv = arg;

  work_queue(HPWORK, &priv->work, cst3530_worker, priv, 0);
  return OK;
}

/****************************************************************************
 * Name: cst3530_open
 *
 * Description:
 *   Wake the controller for the first file opened.
 *
 ****************************************************************************/

static int cst3530_open(FAR struct touch_lowerhalf_s *lower)
{
  FAR struct cst3530_dev_s *priv = (FAR struct cst3530_dev_s *)lower;
  int ret = OK;

  nxmutex_lock(&priv->lock);

  if (priv->nopen == 0)
    {
      cst3530_reset(priv);
      ret = cst3530_normal(priv);
      if (ret < 0)
        {
          ierr("ERROR: Controller did not wake: %d\n", ret);
          goto out;
        }

      priv->downmap = 0;
      priv->config->enable(priv->config, true);
    }

  priv->nopen++;

out:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: cst3530_close
 *
 * Description:
 *   Put the controller in deep sleep when the last file is closed.
 *
 ****************************************************************************/

static int cst3530_close(FAR struct touch_lowerhalf_s *lower)
{
  FAR struct cst3530_dev_s *priv = (FAR struct cst3530_dev_s *)lower;

  nxmutex_lock(&priv->lock);

  if (priv->nopen > 0 && --priv->nopen == 0)
    {
      priv->config->enable(priv->config, false);
      work_cancel(HPWORK, &priv->work);
      cst3530_sleep(priv);
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cst3530_register
 *
 * Description:
 *   Register a CST3530 as a touchscreen.  See include/nuttx/input/cst3530.h.
 *
 ****************************************************************************/

int cst3530_register(FAR const char *devpath, FAR struct i2c_master_s *i2c,
                     uint8_t addr, FAR const struct cst3530_config_s *config)
{
  FAR struct cst3530_dev_s *priv;
  uint8_t info[CST3530_INFO_SIZE];
  int ret = -ENODEV;
  int i;

  DEBUGASSERT(devpath != NULL && i2c != NULL && config != NULL &&
              config->attach != NULL && config->enable != NULL &&
              config->reset != NULL);

  priv = kmm_zalloc(sizeof(struct cst3530_dev_s));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->i2c                 = i2c;
  priv->config              = config;
  priv->i2cconfig.frequency = config->frequency;
  priv->i2cconfig.address   = addr;
  priv->i2cconfig.addrlen   = 7;

  nxmutex_init(&priv->lock);

  /* Read the info block, which starts with a signature that tells this
   * family from other Hynitron parts at the same address, then leave the
   * controller asleep until the device is opened.
   */

  cst3530_reset(priv);

  for (i = 0; i < CST3530_INFO_TRIES; i++)
    {
      if (i > 0)
        {
          nxsched_usleep(CST3530_RETRY_US);
        }

      if (cst3530_normal(priv) >= 0 &&
          cst3530_read(priv, CST3530_REG_INFO, info, sizeof(info)) >= 0 &&
          info[2] == CST3530_INFO_MAGIC && info[3] == CST3530_INFO_MAGIC)
        {
          ret = OK;
          break;
        }
    }

  if (ret < 0)
    {
      ierr("ERROR: No CST3530 at 0x%02x\n", addr);
      goto errout;
    }

  priv->lower.xres = info[28] | ((uint16_t)info[29] << 8);
  priv->lower.yres = info[30] | ((uint16_t)info[31] << 8);

  iinfo("Chip 0x%08lx, project 0x%08lx, firmware 0x%08lx, %ux%u, "
        "%u keys\n",
        (unsigned long)(info[0] | (info[1] << 8) |
                        ((uint32_t)info[2] << 16) |
                        ((uint32_t)info[3] << 24)),
        (unsigned long)(info[36] | (info[37] << 8) |
                        ((uint32_t)info[38] << 16) |
                        ((uint32_t)info[39] << 24)),
        (unsigned long)(info[32] | (info[33] << 8) |
                        ((uint32_t)info[34] << 16) |
                        ((uint32_t)info[35] << 24)),
        priv->lower.xres, priv->lower.yres, info[27]);

  cst3530_sleep(priv);

  priv->lower.maxpoint = CST3530_MAXRECORDS;
  priv->lower.flags    = config->flags;
  priv->lower.open     = cst3530_open;
  priv->lower.close    = cst3530_close;

  ret = config->attach(config, cst3530_interrupt, priv);
  if (ret < 0)
    {
      ierr("ERROR: Failed to attach the interrupt: %d\n", ret);
      goto errout;
    }

  ret = touch_register(&priv->lower, devpath, CST3530_BUFFERS);
  if (ret < 0)
    {
      ierr("ERROR: touch_register failed: %d\n", ret);
      goto errout;
    }

  return OK;

errout:
  nxmutex_destroy(&priv->lock);
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_INPUT_CST3530 */
