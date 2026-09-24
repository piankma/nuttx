/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_sleep.c
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
 * Automatic light sleep from the idle loop.
 *
 * The chip sleeps until whichever comes first of the scheduler's next timer
 * and the next esp_timer alarm, or until a GPIO wake-up source is active.
 * The HAL's esp_light_sleep_start() does the work, including correcting the
 * system timer for the time spent asleep: esp_timer and the tickless
 * scheduler both count on SYSTIMER unit 0, so NuttX's time stays right too.
 * An alarm that the correction jumps past still fires, because the S3's
 * SYSTIMER raises alarms whose target is already behind the counter.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#ifdef CONFIG_ESP32S3_AUTO_SLEEP_CPU_PD
#  include "esp_private/sleep_cpu.h"
#endif

#include "esp32s3_sleep.h"
#include "esp32s3_tickless.h"

#ifdef CONFIG_ESP32S3_AUTO_SLEEP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Wake up this much before the next timer is due, so that the timer is
 * not late when the wake-up takes a little longer than the HAL expects.
 */

#define ESP32S3_SLEEP_EARLY_US  500

/****************************************************************************
 * Private Data
 ****************************************************************************/

static spinlock_t g_sleep_lock = SP_UNLOCKED;

/* The chip starts out held awake, until the board has its wake-up sources
 * and holds in place and releases this first hold.
 */

static int g_sleep_holds = 1;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_sleep_initialize
 ****************************************************************************/

void esp32s3_sleep_initialize(void)
{
  /* One of the HAL's startup functions sets every pin to switch to an
   * isolated, floating configuration during sleep.  That suits a chip that
   * powers its peripherals down, but here chip selects would float next to
   * a floating SPI clock, and outputs that hold other chips in a state
   * would let go.  Keep every pin as it is instead, as waiting for an
   * interrupt does.
   */

  esp_sleep_enable_gpio_switch(false);

#ifdef CONFIG_ESP32S3_AUTO_SLEEP_CPU_PD
  /* Allocate the memory the RTC controller saves the CPU's and the caches'
   * state into; light sleep powers the CPU down once it exists.
   * esp_pm_configure() frees it when light sleep is not enabled there, so
   * this has to come after frequency scaling has called it.
   */

  if (sleep_cpu_configure(true) != ESP_OK)
    {
      serr("ERROR: No memory to power the CPU down in light sleep\n");
    }
#endif
}

/****************************************************************************
 * Name: esp32s3_sleep_hold
 ****************************************************************************/

void esp32s3_sleep_hold(void)
{
  irqstate_t flags = spin_lock_irqsave(&g_sleep_lock);

  g_sleep_holds++;

  spin_unlock_irqrestore(&g_sleep_lock, flags);
}

/****************************************************************************
 * Name: esp32s3_sleep_release
 ****************************************************************************/

void esp32s3_sleep_release(void)
{
  irqstate_t flags = spin_lock_irqsave(&g_sleep_lock);

  DEBUGASSERT(g_sleep_holds > 0);
  if (g_sleep_holds > 0)
    {
      g_sleep_holds--;
    }

  spin_unlock_irqrestore(&g_sleep_lock, flags);
}

/****************************************************************************
 * Name: esp32s3_sleep_wake_on_gpio
 ****************************************************************************/

int esp32s3_sleep_wake_on_gpio(int pin, bool high)
{
  esp_err_t err;

  err = gpio_wakeup_enable(pin, high ? GPIO_INTR_HIGH_LEVEL :
                                      GPIO_INTR_LOW_LEVEL);
  if (err == ESP_OK)
    {
      err = esp_sleep_enable_gpio_wakeup();
    }

  return err == ESP_OK ? OK : -EINVAL;
}

/****************************************************************************
 * Name: esp32s3_sleep_idle
 ****************************************************************************/

bool esp32s3_sleep_idle(void)
{
  irqstate_t flags;
  uint64_t sleep_us;
  int64_t alarm;
  int64_t now;

  /* With interrupts off from here on, nothing can become due between the
   * decision and the sleep.  An interrupt that is raised in the meantime
   * stays pending and is serviced once the chip wakes up.
   */

  flags = up_irq_save();

  if (g_sleep_holds > 0)
    {
      up_irq_restore(flags);
      return false;
    }

  sleep_us = esp32s3_tickless_next();

  alarm = esp_timer_get_next_alarm_for_wake_up();
  if (alarm != INT64_MAX)
    {
      now = esp_timer_get_time();
      if (alarm <= now)
        {
          sleep_us = 0;
        }
      else if ((uint64_t)(alarm - now) < sleep_us)
        {
          sleep_us = alarm - now;
        }
    }

  if (sleep_us < CONFIG_ESP32S3_AUTO_SLEEP_MIN_US)
    {
      up_irq_restore(flags);
      return false;
    }

  if (sleep_us == UINT64_MAX)
    {
      /* Nothing is due at all: sleep until a wake-up source */

      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    }
  else
    {
      esp_sleep_enable_timer_wakeup(sleep_us - ESP32S3_SLEEP_EARLY_US);
    }

  esp_light_sleep_start();

  /* The counter was moved on by the time slept, measured with the slow
   * RC clock: it can pass the interval timer's alarm, which would then
   * never fire
   */

  esp32s3_tickless_resync();

  up_irq_restore(flags);
  return true;
}

#endif /* CONFIG_ESP32S3_AUTO_SLEEP */
