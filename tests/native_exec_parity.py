#!/usr/bin/env python3
"""P1-6 end-to-end parity: emitted machine code vs the VM on one program.

Runs tests/zl/valid/native/NativeExecBench.zl twice through zl_language:

  1. through the native execution driver (`--run-native --call sumSquares`),
     which maps the emitted x86-64 bytes and calls them for real; and
  2. as an ordinary VM program, whose main runs the same loop over the same
     arithmetic in the interpreter.

The VM loop total must equal the driver's per-call result times the iteration
count, and the driver must *refuse* a call it cannot honour (`--call main`)
naming what it compiled instead. Wall times are printed for the record; they
are not asserted (the sandbox is shared and timings swing). On any platform
this is not x86-64 Linux the driver refuses by design, so the test skips.
"""

import platform
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FIXTURE = REPO / "tests" / "zl" / "valid" / "native" / "NativeExecBench.zl"
ITERS = "10000"

failures = []


def require(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL  {msg}")


def main() -> int:
    zl = sys.argv[1]
    if sys.platform != "linux" or platform.machine() != "x86_64":
        print(f"SKIP  native execution driver is x86-64 Linux only (host: {sys.platform}/{platform.machine()})")
        return 0

    native = subprocess.run(
        [zl, "--run-native", str(FIXTURE), "--call", "sumSquares", "--int64", "100", "--iters", ITERS],
        capture_output=True, text=True, timeout=300,
    )
    require(native.returncode == 0, f"--run-native exited {native.returncode}: {native.stderr.strip()[:200]}")
    m = re.search(r"native-exec: NativeExecBench\.sumSquares\(int\) result=(\d+) iters=(\d+) native_ms=([\d.]+)", native.stdout)
    require(m is not None, f"--run-native output not in driver form: {native.stdout!r}")

    vm = subprocess.run([zl, str(FIXTURE)], capture_output=True, text=True, timeout=300)
    require(vm.returncode == 0, f"VM run exited {vm.returncode}: {vm.stderr.strip()[:200]}")
    v = re.search(r"vm-exec: NativeExecBench\.sumSquaresVm\(int\) result=(\d+) iters=(\d+) vm_ms=([\d.]+)", vm.stdout)
    require(v is not None, f"VM output not in benchmark form: {vm.stdout!r}")
    require("ok" in vm.stdout.splitlines()[-1:], "fixture did not end with its ok line")

    if m and v:
        per_call, iters_n, nms = int(m.group(1)), int(m.group(2)), float(m.group(3))
        total, iters_v, vms = int(v.group(1)), int(v.group(2)), float(v.group(3))
        require(iters_n == int(ITERS) and iters_v == int(ITERS), f"iteration counts drifted: native {iters_n}, VM {iters_v}")
        require(total == per_call * iters_n, f"tier disagreement: VM total {total} != native {per_call} x {iters_n}")
        print(f"pass  sumSquares(100)={per_call}; VM loop {total}; native {nms:.1f} ms vs VM {vms:.1f} ms over {iters_n} calls")

    refused = subprocess.run(
        [zl, "--run-native", str(FIXTURE), "--call", "main"],
        capture_output=True, text=True, timeout=300,
    )
    require(refused.returncode != 0, "the driver must refuse a function it did not compile")
    require("no native function matches" in refused.stderr and "sumSquares" in refused.stderr,
            f"refusal must name the compiled set, got: {refused.stderr!r}")

    if failures:
        print(f"native-exec parity: {len(failures)} failure(s)")
        return 1
    print("native-exec parity: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
