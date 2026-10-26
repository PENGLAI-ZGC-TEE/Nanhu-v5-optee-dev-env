#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Summarize a QEMU '-d int' log captured while preempt_test runs.

Only the window in which interrupt logging was enabled is analyzed, so enable
it right before starting preempt_test (QEMU monitor: 'log int') and disable it
afterwards ('log guest_errors').

While the hart is in the REE world, non-secure sources are routed to the
S-context, and in this test no secure source is active. Every machine external
interrupt (async cause 11) in the window is therefore a non-secure interrupt
that the secure PLIC routed to M-mode because the hart was in the TEE world,
i.e. an entry into the OpenSBI foreign-interrupt preemption path.

For each such interrupt the next logged trap is inspected: when OpenSBI
preempts, OP-TEE suspends the thread and returns with an S-mode ecall
(sync cause 9) from its core image, with no trap in between.
"""

import argparse
import re
import sys

LINE_RE = re.compile(r"riscv_cpu_do_interrupt: hart:(\d+), async:(\d), "
                     r"cause:([0-9a-fA-F]+), epc:0x([0-9a-fA-F]+)")

CAUSE_S_TIMER = 5
CAUSE_S_EXT = 9
CAUSE_M_EXT = 11
CAUSE_S_ECALL = 9


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("log", help="QEMU log file (e.g. guest_log.txt)")
    ap.add_argument("--tee-base", type=lambda s: int(s, 0),
                    default=0x81000000, help="OP-TEE core image base")
    ap.add_argument("--tee-size", type=lambda s: int(s, 0),
                    default=0x1000000, help="OP-TEE core image size")
    args = ap.parse_args()

    def in_tee_core(epc):
        return args.tee_base <= epc < args.tee_base + args.tee_size

    def is_linux_kernel(epc):
        return epc >= 0xff00000000000000

    def region(epc):
        if in_tee_core(epc):
            return "optee-core"
        if is_linux_kernel(epc):
            return "linux-kernel"
        return "user(TA or Linux)"

    traps = []
    with open(args.log, errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if m:
                traps.append((int(m.group(2)), int(m.group(3), 16),
                              int(m.group(4), 16)))

    if not traps:
        print("no 'riscv_cpu_do_interrupt' lines found; "
              "was interrupt logging enabled ('-d int' / 'log int')?")
        return 1

    m_ext = {}
    followed_by_tee_ecall = 0
    s_timer_in_tee = 0
    s_ext = 0

    for i, (is_async, cause, epc) in enumerate(traps):
        if is_async and cause == CAUSE_M_EXT:
            r = region(epc)
            m_ext[r] = m_ext.get(r, 0) + 1
            if i + 1 < len(traps):
                n_async, n_cause, n_epc = traps[i + 1]
                if (not n_async and n_cause == CAUSE_S_ECALL and
                        in_tee_core(n_epc)):
                    followed_by_tee_ecall += 1
        elif is_async and cause == CAUSE_S_TIMER and in_tee_core(epc):
            s_timer_in_tee += 1
        elif is_async and cause == CAUSE_S_EXT:
            s_ext += 1

    total = sum(m_ext.values())
    print(f"traps in window                    : {len(traps)}")
    print(f"M external interrupts (cause 11)   : {total}")
    for r, n in sorted(m_ext.items()):
        print(f"  interrupted in {r:<18}: {n}")
    print(f"  next trap is OP-TEE S-ecall      : {followed_by_tee_ecall}")
    print(f"S external interrupts (cause 9)    : {s_ext}")
    print(f"S timer interrupts in OP-TEE core  : {s_timer_in_tee}")

    if total == 0:
        print("\nRESULT: no foreign-interrupt preemption observed")
        return 1
    print("\nRESULT: foreign-interrupt preemption path exercised")
    return 0


if __name__ == "__main__":
    sys.exit(main())
