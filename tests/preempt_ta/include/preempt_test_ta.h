/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Shared definitions for the foreign-interrupt preemption test TA and its
 * host application.
 */

#ifndef PREEMPT_TEST_TA_H
#define PREEMPT_TEST_TA_H

#include <stdint.h>

/* 65dca01d-cd82-4b02-98bb-c980a3657545 */
#define PREEMPT_TEST_TA_UUID \
	{ 0x65dca01d, 0xcd82, 0x4b02, \
	  { 0x98, 0xbb, 0xc9, 0x80, 0xa3, 0x65, 0x75, 0x45 } }

/*
 * Spin in TA user mode for a long time.
 *
 * [in]  params[0].value.a  iterations
 * [in]  params[0].value.b  seed
 * [out] params[1].value.a  checksum, low 32 bits
 * [out] params[1].value.b  checksum, high 32 bits
 * [out] params[2].value.a  elapsed time in ms (TEE system time)
 * [out] params[2].value.b  longest gap in ms between two consecutive chunks
 */
#define PREEMPT_TEST_CMD_SPIN		0

/* Iterations between two TEE_GetSystemTime() samples. */
#define PREEMPT_TEST_CHUNK		(1U << 16)

static inline uint64_t preempt_test_rotl(uint64_t x, unsigned int r)
{
	return (x << r) | (x >> (64 - r));
}

/* splitmix64, used only to derive the initial state from the seed */
static inline uint64_t preempt_test_mix(uint64_t *s)
{
	uint64_t z = (*s += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/*
 * Run @iters rounds starting at round @start over a 16-word state.
 *
 * The empty asm statements with "+r" operands force every state word to live
 * in a general-purpose register at the end of each round. When the TA is suspended
 * by a foreign interrupt and later resumed, any register that is not saved or
 * restored correctly changes the final checksum, which the host recomputes in
 * the normal world and compares.
 */
static inline void preempt_test_rounds(uint64_t st[16], uint32_t start,
				       uint32_t iters)
{
	uint64_t a0 = st[0], a1 = st[1], a2 = st[2], a3 = st[3];
	uint64_t a4 = st[4], a5 = st[5], a6 = st[6], a7 = st[7];
	uint64_t a8 = st[8], a9 = st[9], a10 = st[10], a11 = st[11];
	uint64_t a12 = st[12], a13 = st[13], a14 = st[14], a15 = st[15];
	uint32_t i;

	for (i = start; i < start + iters; i++) {
		a0 = preempt_test_rotl(a0 + a1, 7) ^ i;
		a1 = preempt_test_rotl(a1 + a2, 11) ^ a0;
		a2 = preempt_test_rotl(a2 + a3, 13) ^ a1;
		a3 = preempt_test_rotl(a3 + a4, 17) ^ a2;
		a4 = preempt_test_rotl(a4 + a5, 19) ^ a3;
		a5 = preempt_test_rotl(a5 + a6, 23) ^ a4;
		a6 = preempt_test_rotl(a6 + a7, 29) ^ a5;
		a7 = preempt_test_rotl(a7 + a8, 31) ^ a6;
		a8 = preempt_test_rotl(a8 + a9, 37) ^ a7;
		a9 = preempt_test_rotl(a9 + a10, 41) ^ a8;
		a10 = preempt_test_rotl(a10 + a11, 43) ^ a9;
		a11 = preempt_test_rotl(a11 + a12, 47) ^ a10;
		a12 = preempt_test_rotl(a12 + a13, 53) ^ a11;
		a13 = preempt_test_rotl(a13 + a14, 59) ^ a12;
		a14 = preempt_test_rotl(a14 + a15, 61) ^ a13;
		a15 = preempt_test_rotl(a15 + a0, 3) ^ a14;
		/* Split in two: GCC allows at most 30 asm operands. */
		__asm__ volatile("" : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
				 "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7));
		__asm__ volatile("" : "+r"(a8), "+r"(a9), "+r"(a10), "+r"(a11),
				 "+r"(a12), "+r"(a13), "+r"(a14), "+r"(a15));
	}

	st[0] = a0; st[1] = a1; st[2] = a2; st[3] = a3;
	st[4] = a4; st[5] = a5; st[6] = a6; st[7] = a7;
	st[8] = a8; st[9] = a9; st[10] = a10; st[11] = a11;
	st[12] = a12; st[13] = a13; st[14] = a14; st[15] = a15;
}

static inline void preempt_test_init(uint64_t st[16], uint32_t seed)
{
	uint64_t s = seed;
	int i;

	for (i = 0; i < 16; i++)
		st[i] = preempt_test_mix(&s);
}

static inline uint64_t preempt_test_fold(const uint64_t st[16])
{
	uint64_t h = 0;
	int i;

	for (i = 0; i < 16; i++)
		h = preempt_test_rotl(h, 5) ^ st[i];
	return h;
}

#endif /* PREEMPT_TEST_TA_H */
