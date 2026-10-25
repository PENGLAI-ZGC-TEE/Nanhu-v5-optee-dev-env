/* SPDX-License-Identifier: BSD-2-Clause */

#include <preempt_test_ta.h>
#include <tee_internal_api.h>

TEE_Result TA_CreateEntryPoint(void)
{
	return TEE_SUCCESS;
}

void TA_DestroyEntryPoint(void)
{
}

TEE_Result TA_OpenSessionEntryPoint(uint32_t param_types __unused,
				    TEE_Param params[4] __unused,
				    void **sess_ctx __unused)
{
	return TEE_SUCCESS;
}

void TA_CloseSessionEntryPoint(void *sess_ctx __unused)
{
}

static uint32_t time_ms(void)
{
	TEE_Time t = { };

	TEE_GetSystemTime(&t);
	return t.seconds * 1000 + t.millis;
}

static TEE_Result cmd_spin(uint32_t param_types, TEE_Param params[4])
{
	const uint32_t exp = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INPUT,
					     TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_VALUE_OUTPUT,
					     TEE_PARAM_TYPE_NONE);
	uint32_t iters = 0, done = 0, n = 0;
	uint32_t start = 0, prev = 0, now = 0, max_gap = 0;
	uint64_t st[16] = { };
	uint64_t sum = 0;

	if (param_types != exp)
		return TEE_ERROR_BAD_PARAMETERS;

	iters = params[0].value.a;
	preempt_test_init(st, params[0].value.b);

	start = time_ms();
	prev = start;
	while (done < iters) {
		n = iters - done;
		if (n > PREEMPT_TEST_CHUNK)
			n = PREEMPT_TEST_CHUNK;
		preempt_test_rounds(st, done, n);
		done += n;

		now = time_ms();
		if (now - prev > max_gap)
			max_gap = now - prev;
		prev = now;
	}

	sum = preempt_test_fold(st);
	params[1].value.a = (uint32_t)sum;
	params[1].value.b = (uint32_t)(sum >> 32);
	params[2].value.a = now - start;
	params[2].value.b = max_gap;

	return TEE_SUCCESS;
}

TEE_Result TA_InvokeCommandEntryPoint(void *sess_ctx __unused,
				      uint32_t cmd_id, uint32_t param_types,
				      TEE_Param params[4])
{
	switch (cmd_id) {
	case PREEMPT_TEST_CMD_SPIN:
		return cmd_spin(param_types, params);
	default:
		return TEE_ERROR_NOT_SUPPORTED;
	}
}
