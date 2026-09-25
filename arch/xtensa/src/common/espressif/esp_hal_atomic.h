/****************************************************************************
 * arch/xtensa/src/common/espressif/esp_hal_atomic.h
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
 * Force-included into the Espressif HAL's component sources (hal.mk).
 *
 * The HAL initialises its atomics with ATOMIC_VAR_INIT(), which C23
 * removed.  GCC 15 compiles C23 by default, and its <stdatomic.h> then
 * no longer defines it: the HAL did not build.  An atomic is initialised
 * by its value alone, so define it that way where it is missing.
 ****************************************************************************/

#ifndef __ARCH_XTENSA_SRC_COMMON_ESPRESSIF_ESP_HAL_ATOMIC_H
#define __ARCH_XTENSA_SRC_COMMON_ESPRESSIF_ESP_HAL_ATOMIC_H

#include <stdatomic.h>

#ifndef ATOMIC_VAR_INIT
#  define ATOMIC_VAR_INIT(value) (value)
#endif

#endif /* __ARCH_XTENSA_SRC_COMMON_ESPRESSIF_ESP_HAL_ATOMIC_H */
