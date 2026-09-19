# Copyright (c) 2026 John Crispin <john@phrozen.org>
#
# SPDX-License-Identifier: Apache-2.0

"""SPI-NOR self test. Destructive: it erases and rewrites the whole chip.

Run with a scratch part attached, not a board you care about.
"""

import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from spiflasher import Probe, ProbeError
from spiflasher.nor import (
    CMD_PAGE_PROGRAM,
    CMD_SECTOR_ERASE_4K,
    PROGRAM_TIMEOUT_MS,
    NorError,
    SpiNor,
)

FAILURES = []


def check(name, condition, detail=""):
    status = "pass" if condition else "FAIL"
    print(f"  [{status}] {name}{'  ' + detail if detail else ''}", flush=True)
    if not condition:
        FAILURES.append(name)


def section(title):
    print(f"\n{title}", flush=True)


def main():
    with Probe() as probe:
        nor = SpiNor(probe)
        size = nor.size
        blank = b"\xff" * size

        section(f"identity  ({size // 1024} KiB)")
        check("jedec id plausible", nor.manufacturer not in (0x00, 0xFF),
              bytes(nor.jedec_id).hex(" "))
        check("sfdp signature", nor.read_sfdp(0, 4) == b"SFDP")
        check("not busy at rest", not nor.busy())

        section("write enable latch")
        nor.write_enable()
        check("WEL sets", bool(nor.status() & 0x02))
        probe.xfer(b"\x04")
        check("WEL clears on WRDI", not nor.status() & 0x02)

        section("guards")
        for name, fn in (
            ("unaligned write rejected", lambda: nor.write(1, b"\x00" * 4)),
            ("unaligned erase rejected", lambda: nor.erase(1, 4096)),
            ("unsized erase rejected", lambda: nor.erase(0, 100)),
        ):
            try:
                fn()
                check(name, False, "accepted")
            except NorError:
                check(name, True)

        section("erase sizes")
        for addr, length, label in ((0x000000, 4096, "4K"),
                                    (0x008000, 32768, "32K"),
                                    (0x010000, 65536, "64K")):
            start = time.perf_counter()
            nor.erase(addr, length)
            ms = (time.perf_counter() - start) * 1000
            check(f"erase {label}", nor.read(addr, length) == b"\xff" * length,
                  f"{ms:.0f} ms")

        section("device side poll")
        nor.erase(0, 4096)
        pattern = bytes((i * 13 + 5) & 0xFF for i in range(256))
        sr = probe.xfer_poll(bytes([CMD_PAGE_PROGRAM, 0, 0, 0]) + pattern,
                             timeout_ms=PROGRAM_TIMEOUT_MS)
        check("program returns not busy", sr & 0x01 == 0, f"SR1=0x{sr:02x}")
        check("program readback", nor.read(0, 256) == pattern)
        try:
            probe.xfer_poll(bytes([CMD_SECTOR_ERASE_4K, 0, 0x20, 0]), timeout_ms=1)
            check("timeout reported", False, "no error raised")
        except ProbeError:
            check("timeout reported", True)
        nor.wait_ready()
        check("chip settles after timeout", not nor.busy())

        section("read paths")
        nor.erase(0, 65536)
        data = os.urandom(65536)
        nor.write(0, data)
        check("fast read 0Bh", nor.read(0, 65536) == data)
        check("slow read 03h", SpiNor(probe, fast_read=False).read(0, 65536) == data)
        depths = {d: nor.read(0, 65536, depth=d) == data for d in (1, 2, 3, 4)}
        check("read at every pipeline depth", all(depths.values()), str(depths))

        section("skip semantics")
        nor.write(0, b"\xff" * 256)
        check("all-ones page is skipped", nor.read(0, 256) == data[:256])

        section("whole chip")
        rnd = random.Random(1234)
        payload = bytes(rnd.getrandbits(8) for _ in range(size))

        start = time.perf_counter()
        nor.erase(0, size)
        erase_s = time.perf_counter() - start
        check("erase whole chip", nor.read(0, size) == blank, f"{erase_s:.1f} s")

        start = time.perf_counter()
        nor.write(0, payload)
        write_s = time.perf_counter() - start

        start = time.perf_counter()
        readback = nor.read(0, size)
        read_s = time.perf_counter() - start
        check("write whole chip", readback == payload,
              f"write {write_s:.1f} s ({size / write_s / 1000:.0f} KB/s), "
              f"read {read_s:.1f} s ({size / read_s / 1000:.0f} KB/s)")

        nor.erase(0, size)
        check("erase back to blank", nor.read(0, size) == blank)

    print()
    if FAILURES:
        print(f"FAILED: {len(FAILURES)} check(s): {', '.join(FAILURES)}")
        return 1
    print("all checks passed")
    return 0


sys.exit(main())
