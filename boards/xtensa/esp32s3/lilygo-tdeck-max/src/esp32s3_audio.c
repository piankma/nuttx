/****************************************************************************
 * boards/xtensa/esp32s3/lilygo-tdeck-max/src/esp32s3_audio.c
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
 * The ES8311 codec is on the shared I2C bus at 0x18 and on I2S0: MCLK
 * GPIO38, BCLK GPIO39, WS GPIO18, data to the codec GPIO40, from it GPIO17
 * (schematic page 8; the vendor's pin table and macros have these two the
 * other way round).  Its output drives the speaker through an amplifier
 * switched by the XL9555's amp_en line; the XL9555's audio_sel line
 * connects the codec rather than the modem (low, from boot).  The
 * microphone is an analogue electret on MIC1P/MIC1N, biased from ADCVREF.
 *
 * Playback is /dev/audio/pcm0, through NuttX's PCM decoder so that WAV
 * files play; recording is /dev/audio/pcm_in0.  The amplifier is switched
 * on while the output is reserved by a player, and the chip is held out of
 * light sleep while either is (I2S stops in light sleep; the amplifier's
 * rail holds it for playback).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/i2c/i2c_master.h>

#include <arch/board/board.h>

#include "espressif/esp_i2s.h"
#include "esp32s3_i2c.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP
#  include "esp32s3_sleep.h"
#endif

#include "lilygo-tdeck-max.h"

#if defined(CONFIG_AUDIO_ES8311) && defined(CONFIG_ESPRESSIF_I2S0)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_AUDIO_MULTI_SESSION
#  define SESSION_ARG   , FAR void **session
#  define SESSION_PASS  , session
#  define RSESSION_ARG  , FAR void *session
#else
#  define SESSION_ARG
#  define SESSION_PASS
#  define RSESSION_ARG
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct es8311_lower_s g_es8311_out =
{
  .frequency = 400000,
  .address   = BOARD_I2C_ADDR_ES8311,
};

static const struct es8311_lower_s g_es8311_in =
{
  .frequency = 400000,
  .address   = BOARD_I2C_ADDR_ES8311,
};

/* The playback and recording devices' operations, with reserve and release
 * extended to switch the amplifier and hold the chip awake
 */

static struct audio_ops_s g_out_ops;
static FAR const struct audio_ops_s *g_out_orig;
static struct audio_ops_s g_in_ops;
static FAR const struct audio_ops_s *g_in_orig;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_out_reserve / tdeckmax_out_release
 ****************************************************************************/

static int tdeckmax_out_reserve(FAR struct audio_lowerhalf_s *dev
                                SESSION_ARG)
{
  int ret = g_out_orig->reserve(dev SESSION_PASS);

  if (ret >= 0)
    {
      tdeckmax_xl9555_rail(XL9555_PIN_AMP_EN, true);
    }

  return ret;
}

static int tdeckmax_out_release(FAR struct audio_lowerhalf_s *dev
                                RSESSION_ARG)
{
  int ret = g_out_orig->release(dev SESSION_PASS);

  tdeckmax_xl9555_rail(XL9555_PIN_AMP_EN, false);
  return ret;
}

/****************************************************************************
 * Name: tdeckmax_in_reserve / tdeckmax_in_release
 ****************************************************************************/

static int tdeckmax_in_reserve(FAR struct audio_lowerhalf_s *dev
                               SESSION_ARG)
{
  int ret = g_in_orig->reserve(dev SESSION_PASS);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  if (ret >= 0)
    {
      esp32s3_sleep_hold();
    }
#endif

  return ret;
}

static int tdeckmax_in_release(FAR struct audio_lowerhalf_s *dev
                               RSESSION_ARG)
{
  int ret = g_in_orig->release(dev SESSION_PASS);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP
  esp32s3_sleep_release();
#endif

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: tdeckmax_audio_initialize
 *
 * Description:
 *   See src/lilygo-tdeck-max.h
 *
 ****************************************************************************/

int tdeckmax_audio_initialize(void)
{
  FAR struct audio_lowerhalf_s *codec;
  FAR struct audio_lowerhalf_s *pcm;
  FAR struct i2c_master_s *i2c;
  FAR struct i2s_dev_s *i2s;
  int ret;

  i2c = esp32s3_i2cbus_initialize(TDECKMAX_I2C_PORT);
  i2s = esp_i2sbus_initialize(ESP32S3_I2S0);
  if (i2c == NULL || i2s == NULL)
    {
      return -ENODEV;
    }

  /* Playback: codec, PCM decoder for WAV files, amplifier while reserved */

  codec = es8311_initialize(i2c, i2s, &g_es8311_out);
  pcm   = codec != NULL ? pcm_decode_initialize(codec) : NULL;
  if (pcm == NULL)
    {
      auderr("ERROR: Failed to set up the ES8311 output\n");
      return -ENODEV;
    }

  g_out_orig        = pcm->ops;
  g_out_ops         = *pcm->ops;
  g_out_ops.reserve = tdeckmax_out_reserve;
  g_out_ops.release = tdeckmax_out_release;
  pcm->ops          = &g_out_ops;

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      auderr("ERROR: Failed to register pcm0: %d\n", ret);
      return ret;
    }

  /* Recording: the codec's microphone input */

  codec = es8311_initialize(i2c, i2s, &g_es8311_in);
  if (codec == NULL)
    {
      auderr("ERROR: Failed to set up the ES8311 input\n");
      return -ENODEV;
    }

  g_in_orig        = codec->ops;
  g_in_ops         = *codec->ops;
  g_in_ops.reserve = tdeckmax_in_reserve;
  g_in_ops.release = tdeckmax_in_release;
  codec->ops       = &g_in_ops;

  ret = audio_register("pcm_in0", codec);
  if (ret < 0)
    {
      auderr("ERROR: Failed to register pcm_in0: %d\n", ret);
    }

  return ret;
}

#endif /* CONFIG_AUDIO_ES8311 && CONFIG_ESPRESSIF_I2S0 */
