# Copyright (c) 2026 John Crispin <john@phrozen.org>
#
# SPDX-License-Identifier: Apache-2.0

"""Transport benchmark. TEST_IN moves bytes without touching SPI, so it
measures the USB path alone."""

import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from spiflasher import Probe


def bench(probe, fn, size, seconds=1.0):
    fn()
    calls = 0
    start = time.perf_counter()
    while time.perf_counter() - start < seconds:
        fn()
        calls += 1
    elapsed = time.perf_counter() - start
    per_call = elapsed / calls
    rate = size * calls / elapsed
    print(f"{size:6d} B  {per_call * 1000:8.3f} ms/call  {rate / 1000:9.1f} KB/s")
    return per_call


with Probe() as p:
    print(f"proto v{p.version}, max transfer {p.max_xfer} B, SPI {p.frequency} Hz")
    print()
    print("USB only (TEST_IN, no SPI):")
    times = {}
    for n in (64, 256, 1024, 2048, 4096, 4608):
        times[n] = bench(p, lambda n=n: p.test_in(n), n)

    floor = bench(p, lambda: p.command(0x00, rx_len=10), 10)
    slope = (times[4096] - times[1024]) / (4096 - 1024)
    print()
    print(f"latency floor     {floor * 1000:8.3f} ms")
    print(f"marginal per byte {slope * 1e6:8.3f} us")
    print(f"link throughput   {1 / slope / 1000:8.1f} KB/s")
