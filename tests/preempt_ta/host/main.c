// SPDX-License-Identifier: BSD-2-Clause
/*
 * Host side of the foreign-interrupt preemption test.
 *
 * Runs the preempt_test TA's long user-mode spin loop and checks the result
 * against a normal-world recomputation. To exercise the preemption path, type
 * on the console while the TA runs: UART0 RX interrupts (IRQ40, listed in
 * "riscv,ree-preempt-irqs") then arrive while the hart is in the TEE world.
 * Interrupts caused by console output are raised synchronously by Linux in
 * the REE world and do not exercise the path.
 *
 * The TEE is driven through the Linux TEE subsystem ioctl ABI directly, so the
 * binary can be linked statically and does not depend on the libteec/glibc
 * versions shipped in the rootfs.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/tee.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <preempt_test_ta.h>

struct ta_uuid {
	uint32_t time_low;
	uint16_t time_mid;
	uint16_t time_hi_and_version;
	uint8_t clock_seq_and_node[8];
};

static void uuid_to_octets(uint8_t d[16], const struct ta_uuid *u)
{
	d[0] = u->time_low >> 24;
	d[1] = u->time_low >> 16;
	d[2] = u->time_low >> 8;
	d[3] = u->time_low;
	d[4] = u->time_mid >> 8;
	d[5] = u->time_mid;
	d[6] = u->time_hi_and_version >> 8;
	d[7] = u->time_hi_and_version;
	memcpy(d + 8, u->clock_seq_and_node, 8);
}

static int open_session(int fd, uint32_t *session)
{
	const struct ta_uuid uuid = PREEMPT_TEST_TA_UUID;
	struct tee_ioctl_open_session_arg arg = { };
	struct tee_ioctl_buf_data buf = {
		.buf_ptr = (uintptr_t)&arg,
		.buf_len = sizeof(arg),
	};

	uuid_to_octets(arg.uuid, &uuid);
	arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	arg.num_params = 0;

	if (ioctl(fd, TEE_IOC_OPEN_SESSION, &buf)) {
		perror("TEE_IOC_OPEN_SESSION");
		return -1;
	}
	if (arg.ret) {
		fprintf(stderr, "open session: ret 0x%x origin %u\n",
			arg.ret, arg.ret_origin);
		return -1;
	}

	*session = arg.session;
	return 0;
}

static void close_session(int fd, uint32_t session)
{
	struct tee_ioctl_close_session_arg arg = { .session = session };

	ioctl(fd, TEE_IOC_CLOSE_SESSION, &arg);
}

struct spin_result {
	uint64_t sum;
	uint32_t elapsed_ms;
	uint32_t max_gap_ms;
};

static int invoke_spin(int fd, uint32_t session, uint32_t iters,
		       uint32_t seed, struct spin_result *res)
{
	struct {
		struct tee_ioctl_invoke_arg arg;
		struct tee_ioctl_param params[3];
	} inv = { };
	struct tee_ioctl_buf_data buf = {
		.buf_ptr = (uintptr_t)&inv,
		.buf_len = sizeof(inv),
	};

	inv.arg.func = PREEMPT_TEST_CMD_SPIN;
	inv.arg.session = session;
	inv.arg.num_params = 3;
	inv.params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	inv.params[0].a = iters;
	inv.params[0].b = seed;
	inv.params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	inv.params[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	if (ioctl(fd, TEE_IOC_INVOKE, &buf)) {
		perror("TEE_IOC_INVOKE");
		return -1;
	}
	if (inv.arg.ret) {
		fprintf(stderr, "invoke: ret 0x%x origin %u\n",
			inv.arg.ret, inv.arg.ret_origin);
		return -1;
	}

	res->sum = (uint64_t)(uint32_t)inv.params[1].b << 32 |
		   (uint32_t)inv.params[1].a;
	res->elapsed_ms = inv.params[2].a;
	res->max_gap_ms = inv.params[2].b;
	return 0;
}

static uint64_t expected_sum(uint32_t iters, uint32_t seed)
{
	uint64_t st[16];

	preempt_test_init(st, seed);
	preempt_test_rounds(st, 0, iters);
	return preempt_test_fold(st);
}

/* Sum of all per-CPU counts of /proc/interrupts lines naming a ttyS port. */
static long uart_irq_count(void)
{
	char line[512];
	long total = -1;
	FILE *f = fopen("/proc/interrupts", "r");

	if (!f)
		return -1;

	while (fgets(line, sizeof(line), f)) {
		char *p = strchr(line, ':');

		if (!p || !strstr(line, "ttyS"))
			continue;
		if (total < 0)
			total = 0;
		for (p++; *p;) {
			char *end;
			long v = strtol(p, &end, 10);

			if (end == p)
				break;
			total += v;
			p = end;
		}
	}

	fclose(f);
	return total;
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-n iterations] [-r rounds]\n"
		"  -n  spin iterations per round (default 200000000)\n"
		"  -r  number of rounds (default 3)\n",
		prog);
}

int main(int argc, char *argv[])
{
	uint32_t iters = 200000000, rounds = 3, session = 0, r;
	long irq_before, irq_after;
	int fd, opt, failed = 0;

	while ((opt = getopt(argc, argv, "n:r:h")) != -1) {
		switch (opt) {
		case 'n':
			iters = strtoul(optarg, NULL, 0);
			break;
		case 'r':
			rounds = strtoul(optarg, NULL, 0);
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	fd = open("/dev/tee0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/tee0");
		return 1;
	}
	if (open_session(fd, &session)) {
		close(fd);
		return 1;
	}

	for (r = 0; r < rounds; r++) {
		struct spin_result res = { };
		uint32_t seed = r + 1;
		double t0, t1;
		uint64_t exp;

		printf("[round %u] iterations %u seed %u\n", r, iters, seed);
		fflush(stdout);

		irq_before = uart_irq_count();

		t0 = now_s();
		if (invoke_spin(fd, session, iters, seed, &res)) {
			failed = 1;
			break;
		}
		t1 = now_s();
		irq_after = uart_irq_count();

		exp = expected_sum(iters, seed);
		printf("[round %u] wall %.2f s, TA elapsed %u ms, "
		       "TA max chunk gap %u ms, ttyS IRQs %ld\n",
		       r, t1 - t0, res.elapsed_ms, res.max_gap_ms,
		       (irq_before >= 0 && irq_after >= 0) ?
		       irq_after - irq_before : -1L);
		printf("[round %u] checksum 0x%016" PRIx64 " expected 0x%016"
		       PRIx64 " %s\n", r, res.sum, exp,
		       res.sum == exp ? "OK" : "MISMATCH");
		if (res.sum != exp)
			failed = 1;
	}

	close_session(fd, session);
	close(fd);

	printf("preempt_test: %s\n", failed ? "FAILED" : "PASSED");
	return failed;
}
