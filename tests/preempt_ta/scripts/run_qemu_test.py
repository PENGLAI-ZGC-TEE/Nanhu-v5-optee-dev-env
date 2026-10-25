#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Boot Nanhu V5 Linux + OP-TEE in QEMU and run preempt_test unattended.

Interrupt logging ('log int') is enabled through the QEMU monitor only while
preempt_test runs, then analyze_int_log.py summarizes the captured window.
Run from the repository root after 'make preempt-test-install linux-image'.
"""

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))
B = os.path.join(ROOT, "build")


class Console:
    def __init__(self, cmd, transcript):
        self.master, slave = pty.openpty()
        self.proc = subprocess.Popen(cmd, stdin=slave, stdout=slave,
                                     stderr=slave, cwd=ROOT)
        os.close(slave)
        self.buf = b""
        self.transcript = open(transcript, "wb")

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master, data)

    def expect(self, pattern, timeout, tick=None, interval=None):
        regex = re.compile(pattern.encode() if isinstance(pattern, str)
                           else pattern)
        deadline = time.time() + timeout
        next_tick = time.time()
        while True:
            if tick and time.time() >= next_tick:
                tick()
                next_tick = time.time() + interval
            m = regex.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            left = deadline - time.time()
            if left <= 0 or self.proc.poll() is not None:
                raise TimeoutError(f"timed out waiting for {pattern!r}")
            wait = min(left, interval if tick else 1.0)
            r, _, _ = select.select([self.master], [], [], wait)
            if r:
                try:
                    data = os.read(self.master, 65536)
                except OSError:
                    data = b""
                if data:
                    self.buf += data
                    self.transcript.write(data)
                    self.transcript.flush()
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()

    def shell(self, cmd, timeout=60):
        tag = f"{int(time.time() * 1000)}__"
        # The echoed command line contains __DO""NE_..., only the output of
        # echo contains the contiguous marker.
        self.send(f'{cmd}; echo __DO""NE_{tag}\n')
        return self.expect(re.escape("__DONE_" + tag), timeout)

    def monitor(self, cmd):
        self.send(b"\x01c")
        self.expect(r"\(qemu\)", 10)
        self.send(cmd + "\n")
        self.expect(r"\(qemu\)", 10)
        self.send(b"\x01c")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-n", "--iterations", default="200000000")
    ap.add_argument("-r", "--rounds", default="3")
    ap.add_argument("--no-typing", action="store_true",
                    help="do not type on the console while the TA runs")
    ap.add_argument("--typing-interval", type=float, default=0.02,
                    help="seconds between typed characters (default 0.02)")
    ap.add_argument("--no-int-log", action="store_true",
                    help="do not capture the QEMU interrupt log")
    ap.add_argument("--log", default=os.path.join(ROOT, "guest_log.txt"))
    ap.add_argument("--transcript",
                    default=os.path.join(B, "preempt_test", "console.log"))
    ap.add_argument("--boot-timeout", type=int, default=600)
    ap.add_argument("--test-timeout", type=int, default=1800)
    args = ap.parse_args()

    cmd = [os.path.join(B, "qemu", "qemu-system-riscv64"),
           "-machine", "xiangshan-nanhuv5",
           "-cpu", "xiangshan-nanhuv5", "-smp", "1", "-m", "8G",
           "-d", "guest_errors", "-D", args.log,
           "-bios", os.path.join(B, "opensbi/platform/generic/firmware/"
                                    "fw_jump.bin"),
           "-device", "loader,file=" + os.path.join(B, "optee_os/core/"
                                                    "tee.bin") +
           ",addr=0x81000000",
           "-device", "loader,file=" + os.path.join(B, "linux/arch/riscv/"
                                                    "boot/Image") +
           ",addr=0x82000000",
           "-nographic"]

    if os.path.exists(args.log):
        os.remove(args.log)
    os.makedirs(os.path.dirname(args.transcript), exist_ok=True)

    con = Console(cmd, args.transcript)
    status = 1
    try:
        m = con.expect(r"login:|# ", args.boot_timeout)
        if m.group(0) == b"login:":
            con.send("root\n")
            con.expect(r"# ", 60)
        # Let init finish (tee-supplicant) and settle the console.
        time.sleep(5)
        con.shell("pidof tee-supplicant; ls /lib/optee_armtz/"
                  "65dca01d-cd82-4b02-98bb-c980a3657545.ta")
        con.shell("grep ttyS /proc/interrupts")
        # PLIC enable words holding IRQ40/41: context 0 is M, context 1 is S.
        con.shell("devmem 0x3c002004 32; devmem 0x3c002084 32")

        if not args.no_int_log:
            con.monitor("log int")
        test = f"preempt_test -n {args.iterations} -r {args.rounds}"
        con.send(test + "\n")
        # Type on the console while the TA runs: UART0 RX interrupts arrive
        # asynchronously, i.e. mostly while the hart is in the TEE world.
        tick = None if args.no_typing else (lambda: con.send(" "))
        m = con.expect(r"preempt_test: (PASSED|FAILED)", args.test_timeout,
                       tick, args.typing_interval)
        passed = m.group(1) == b"PASSED"
        # Drop the typed spaces from the shell's input line.
        con.send(b"\x15")
        if not args.no_int_log:
            con.monitor("log guest_errors")
        con.shell("grep ttyS /proc/interrupts")
        status = 0 if passed else 1
    finally:
        con.send(b"\x01x")
        try:
            con.proc.wait(10)
        except subprocess.TimeoutExpired:
            con.proc.kill()

    print("\n==== preempt_test:", "PASSED" if status == 0 else "FAILED")
    if not args.no_int_log:
        print("==== QEMU interrupt log analysis")
        rc = subprocess.call([sys.executable,
                              os.path.join(os.path.dirname(__file__),
                                           "analyze_int_log.py"), args.log])
        if rc:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
