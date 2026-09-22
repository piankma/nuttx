/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_pwm.c
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
 * The two backlights are LEDC PWM channels on /dev/pwm0: the e-paper
 * frontlight on channel 1 and the keyboard backlight on channel 2.  The
 * LEDC runs from the APB clock, which stops in light sleep, so the output
 * would stop with it.  The LEDC lower half is wrapped here to hold the chip
 * awake from the start of the output until it is stopped or the device is
 * closed.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/timers/pwm.h>

#include "espressif/esp_ledc.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

#ifdef CONFIG_PWM

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tdeckmax_pwm_s
{
  struct pwm_lowerhalf_s lower;       /* Must be first */
  FAR struct pwm_lowerhalf_s *ledc;   /* The LEDC lower half */
  bool held;                          /* Holding the chip awake */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int tdeckmax_pwm_setup(FAR struct pwm_lowerhalf_s *dev);
static int tdeckmax_pwm_shutdown(FAR struct pwm_lowerhalf_s *dev);
static int tdeckmax_pwm_start(FAR struct pwm_lowerhalf_s *dev,
                              FAR const struct pwm_info_s *info);
static int tdeckmax_pwm_stop(FAR struct pwm_lowerhalf_s *dev);
static int tdeckmax_pwm_ioctl(FAR struct pwm_lowerhalf_s *dev, int cmd,
                              unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pwm_ops_s g_tdeckmax_pwm_ops =
{
  .setup    = tdeckmax_pwm_setup,
  .shutdown = tdeckmax_pwm_shutdown,
  .start    = tdeckmax_pwm_start,
  .stop     = tdeckmax_pwm_stop,
  .ioctl    = tdeckmax_pwm_ioctl,
};

static struct tdeckmax_pwm_s g_tdeckmax_pwm =
{
  .lower =
    {
      .ops = &g_tdeckmax_pwm_ops,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_pwm_awake
 *
 * Description:
 *   Hold the chip awake while the output runs, and let it sleep again.
 *
 ****************************************************************************/

static void tdeckmax_pwm_awake(FAR struct tdeckmax_pwm_s *priv, bool on)
{
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  if (on != priv->held)
    {
      if (on)
        {
          esp32s3_sleep_hold();
        }
      else
        {
          esp32s3_sleep_release();
        }

      priv->held = on;
    }
#endif
}

/****************************************************************************
 * Name: tdeckmax_pwm_setup
 ****************************************************************************/

static int tdeckmax_pwm_setup(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct tdeckmax_pwm_s *priv = (FAR struct tdeckmax_pwm_s *)dev;

  return priv->ledc->ops->setup(priv->ledc);
}

/****************************************************************************
 * Name: tdeckmax_pwm_shutdown
 ****************************************************************************/

static int tdeckmax_pwm_shutdown(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct tdeckmax_pwm_s *priv = (FAR struct tdeckmax_pwm_s *)dev;
  int ret;

  ret = priv->ledc->ops->shutdown(priv->ledc);
  tdeckmax_pwm_awake(priv, false);
  return ret;
}

/****************************************************************************
 * Name: tdeckmax_pwm_start
 ****************************************************************************/

static int tdeckmax_pwm_start(FAR struct pwm_lowerhalf_s *dev,
                              FAR const struct pwm_info_s *info)
{
  FAR struct tdeckmax_pwm_s *priv = (FAR struct tdeckmax_pwm_s *)dev;
  int ret;

  /* Stay awake from before the output starts */

  tdeckmax_pwm_awake(priv, true);

  ret = priv->ledc->ops->start(priv->ledc, info);
  if (ret < 0)
    {
      tdeckmax_pwm_awake(priv, false);
    }

  return ret;
}

/****************************************************************************
 * Name: tdeckmax_pwm_stop
 ****************************************************************************/

static int tdeckmax_pwm_stop(FAR struct pwm_lowerhalf_s *dev)
{
  FAR struct tdeckmax_pwm_s *priv = (FAR struct tdeckmax_pwm_s *)dev;
  int ret;

  ret = priv->ledc->ops->stop(priv->ledc);
  tdeckmax_pwm_awake(priv, false);
  return ret;
}

/****************************************************************************
 * Name: tdeckmax_pwm_ioctl
 ****************************************************************************/

static int tdeckmax_pwm_ioctl(FAR struct pwm_lowerhalf_s *dev, int cmd,
                              unsigned long arg)
{
  FAR struct tdeckmax_pwm_s *priv = (FAR struct tdeckmax_pwm_s *)dev;

  return priv->ledc->ops->ioctl(priv->ledc, cmd, arg);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_pwm_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_pwm_initialize(void)
{
  g_tdeckmax_pwm.ledc = esp_ledc_init(0);
  if (g_tdeckmax_pwm.ledc == NULL)
    {
      syslog(LOG_ERR, "ERROR: Failed to get the LEDC PWM 0 lower half\n");
      return -ENODEV;
    }

  return pwm_register("/dev/pwm0", &g_tdeckmax_pwm.lower);
}

#endif /* CONFIG_PWM */
