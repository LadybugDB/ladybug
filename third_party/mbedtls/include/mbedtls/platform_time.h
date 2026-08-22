/**
 * \file platform_time.h
 *
 * \brief mbed TLS Platform time abstraction
 */
/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#ifndef MBEDTLS_PLATFORM_TIME_H
#define MBEDTLS_PLATFORM_TIME_H

#include "mbedtls/build_info.h"


/**
 * \name SECTION: Module settings
 *
 * The configuration options you can set for this module are in this section.
 * Either change them in mbedtls_config.h or define them on the compiler command line.
 * \{
 */

/*
 * The time_t datatype
 */
#if defined(MBEDTLS_PLATFORM_TIME_TYPE_MACRO)
namespace lbug_mbedtls {
typedef MBEDTLS_PLATFORM_TIME_TYPE_MACRO mbedtls_time_t;
} // namespace lbug_mbedtls
#else
/* For time_t */
#include <time.h>
namespace lbug_mbedtls {
typedef time_t mbedtls_time_t;
} // namespace lbug_mbedtls
#endif /* MBEDTLS_PLATFORM_TIME_TYPE_MACRO */

/*
 * The function pointers for time
 */
#if defined(MBEDTLS_PLATFORM_TIME_ALT)
namespace lbug_mbedtls {
extern mbedtls_time_t (*mbedtls_time)(mbedtls_time_t* time);

/**
 * \brief   Set your own time function pointer
 *
 * \param   time_func   the time function implementation
 *
 * \return              0
 */
int mbedtls_platform_set_time(mbedtls_time_t (*time_func)(mbedtls_time_t* time));
} // namespace lbug_mbedtls
#else
#if defined(MBEDTLS_PLATFORM_TIME_MACRO)
#define mbedtls_time MBEDTLS_PLATFORM_TIME_MACRO
#else
#define mbedtls_time time
#endif /* MBEDTLS_PLATFORM_TIME_MACRO */
#endif /* MBEDTLS_PLATFORM_TIME_ALT */


#endif /* platform_time.h */

using namespace lbug_mbedtls; // keep unqualified names for in-tree consumers
