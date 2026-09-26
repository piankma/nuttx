/****************************************************************************
 * arch/xtensa/src/esp32s3/esp32s3_sleep.h
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

#ifndef __ARCH_XTENSA_SRC_ESP32S3_ESP32S3_SLEEP_H
#define __ARCH_XTENSA_SRC_ESP32S3_ESP32S3_SLEEP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#ifdef CONFIG_ESP32S3_AUTO_SLEEP

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp32s3_sleep_initialize
 *
 * Description:
 *   Prepare automatic light sleep.  Called once at boot, after the HAL's
 *   startup functions have run.
 *
 *   The chip starts out held awake: the board calls esp32s3_sleep_release()
 *   once, when its wake-up sources and its own holds are in place.
 *
 ****************************************************************************/

void esp32s3_sleep_initialize(void);

/****************************************************************************
 * Name: esp32s3_sleep_hold / esp32s3_sleep_release
 *
 * Description:
 *   Keep the chip out of light sleep, and let it sleep again.  Holds
 *   nest: the chip may sleep once every hold has been released.  Anything
 *   that must keep running while the CPU idles and would stop in light
 *   sleep, such as a PWM output, a UART that may receive data or a USB
 *   connection, holds the chip awake for as long as it needs to.  Both may
 *   be called from interrupt handlers.
 *
 ****************************************************************************/

void esp32s3_sleep_hold(void);
void esp32s3_sleep_release(void);

/****************************************************************************
 * Name: esp32s3_sleep_wake_on_gpio
 *
 * Description:
 *   Wake the chip from light sleep while the pin is at the given level.
 *   Light sleep wake-ups are level triggered, and the pin's interrupt, if
 *   it has one, becomes level triggered too; edges that arrive while the
 *   chip sleeps would be lost.
 *
 * Input Parameters:
 *   pin  - The GPIO
 *   high - true to wake while the pin is high, false while it is low
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp32s3_sleep_wake_on_gpio(int pin, bool high);

/****************************************************************************
 * Name: esp32s3_sleep_wake_on_gpio_off
 *
 * Description:
 *   No longer wake the chip from light sleep on the pin.
 *
 ****************************************************************************/

int esp32s3_sleep_wake_on_gpio_off(int pin);

/****************************************************************************
 * Name: esp32s3_sleep_idle
 *
 * Description:
 *   Called by the idle loop.  If nothing holds the chip awake and nothing
 *   is due for at least CONFIG_ESP32S3_AUTO_SLEEP_MIN_US, sleep until the
 *   next timer or wake-up source.
 *
 * Returned Value:
 *   true if the chip slept, false if the caller should wait for an
 *   interrupt instead.
 *
 ****************************************************************************/

bool esp32s3_sleep_idle(void);

#endif /* CONFIG_ESP32S3_AUTO_SLEEP */
#endif /* __ARCH_XTENSA_SRC_ESP32S3_ESP32S3_SLEEP_H */
