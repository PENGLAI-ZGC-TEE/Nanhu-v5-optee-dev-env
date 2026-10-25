/* SPDX-License-Identifier: BSD-2-Clause */

#ifndef USER_TA_HEADER_DEFINES_H
#define USER_TA_HEADER_DEFINES_H

#include <preempt_test_ta.h>

#define TA_UUID			PREEMPT_TEST_TA_UUID

#define TA_FLAGS		TA_FLAG_EXEC_DDR
#define TA_STACK_SIZE		(4 * 1024)
#define TA_DATA_SIZE		(16 * 1024)

#define TA_VERSION		"1.0"
#define TA_DESCRIPTION		"Foreign-interrupt preemption test TA"

#endif /* USER_TA_HEADER_DEFINES_H */
