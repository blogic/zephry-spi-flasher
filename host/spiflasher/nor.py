# Copyright (c) 2026 John Crispin <john@phrozen.org>
#
# SPDX-License-Identifier: Apache-2.0

"""SPI-NOR (25-series) support.

Command numbers and timings come from the JEDEC-compatible 25-series
datasheets (Winbond W25Q, Macronix MX25L, Micron N25Q and equivalents).
"""

import struct
import time

CMD_WRITE_ENABLE = 0x06
CMD_WRITE_DISABLE = 0x04
CMD_READ_STATUS1 = 0x05
CMD_READ_STATUS2 = 0x35
CMD_WRITE_STATUS = 0x01
CMD_READ = 0x03
CMD_FAST_READ = 0x0B
CMD_PAGE_PROGRAM = 0x02
CMD_SECTOR_ERASE_4K = 0x20
CMD_BLOCK_ERASE_32K = 0x52
CMD_BLOCK_ERASE_64K = 0xD8
CMD_CHIP_ERASE = 0xC7
CMD_READ_ID = 0x9F
CMD_READ_SFDP = 0x5A
CMD_WRITE_STATUS2 = 0x31
CMD_VOLATILE_SR_WRITE_ENABLE = 0x50
CMD_ENTER_4BYTE = 0xB7
CMD_EXIT_4BYTE = 0xE9

SR1_BUSY = 0x01
SR1_WEL = 0x02
SR1_PROTECT = 0x7C   # BP0..BP2, TB, SEC
SR1_SRP0 = 0x80
SR2_QE = 0x02

SFDP_SIGNATURE = b"SFDP"

PAGE_SIZE = 256
MIN_PLAUSIBLE_SIZE = 64 * 1024
MAX_PLAUSIBLE_SIZE = 512 * 1024 * 1024
PROGRAM_TIMEOUT_MS = 100
ERASE_TIMEOUT_MS = 5000
CHIP_ERASE_TIMEOUT_MS = 200000


class NorError(Exception):
    pass


def _addr_bytes(addr, width):
    return addr.to_bytes(width, "big")


class SpiNor:
    def __init__(self, probe, fast_read=True, disable_quad=False):
        self.probe = probe
        self.fast_read = fast_read
        self.jedec_id = self.read_id()
        self.manufacturer, self.memory_type, self.capacity_code = self.jedec_id

        # An all-zero or all-ones ID means nothing is answering. Refuse rather
        # than report a zero-sized chip and hand back buffers full of the
        # MISO bias level, which look like real data.
        if self.manufacturer in (0x00, 0xFF) or not 0x10 <= self.capacity_code <= 0x1C:
            raise NorError(
                f"implausible JEDEC id {bytes(self.jedec_id).hex(' ')}: the chip is "
                "not responding. Tap NRST on the probe and retry."
            )

        self.size = 1 << self.capacity_code

        # SFDP is only trusted when it agrees with the JEDEC capacity code.
        # Two independent sources must concur before capacity is believed,
        # because capacity decides the address width.
        sfdp_size = self.sfdp_density()
        self.sfdp = sfdp_size == self.size

        self.addr_width = 4 if self.size > (1 << 24) else 3

        # Put the address mode in a known state rather than assuming one. A
        # chip left in four byte mode by an earlier run would otherwise
        # misinterpret every address.
        self.probe.xfer(bytes([CMD_ENTER_4BYTE if self.addr_width == 4
                               else CMD_EXIT_4BYTE]))

        self.quad_disabled = self.quad_disable() if disable_quad else False

    def read_id(self):
        return tuple(self.probe.xfer(bytes([CMD_READ_ID]), rx_len=3))

    def read_sfdp(self, addr=0, length=8):
        tx = bytes([CMD_READ_SFDP]) + _addr_bytes(addr, 3) + b"\x00"
        return self.probe.xfer(tx, rx_len=length)

    def sfdp_density(self):
        """Capacity in bytes from the SFDP basic flash parameter table, or
        None if the chip has no usable SFDP. More trustworthy than inferring
        it from the third JEDEC id byte."""
        header = self.read_sfdp(0, 16)
        if header[:4] != SFDP_SIGNATURE:
            return None

        # First parameter header follows the 8 byte SFDP header: id, minor,
        # major, length in dwords, then a 24 bit pointer.
        if header[8] != 0x00:
            return None
        ptr = int.from_bytes(header[12:15], "little")

        bfpt = self.read_sfdp(ptr, 8)
        density = int.from_bytes(bfpt[4:8], "little")

        if density & (1 << 31):
            exponent = density & 0x7FFFFFFF
            # Refuse to shift by an unbounded amount. A corrupt read here
            # would otherwise produce an absurd capacity, and capacity decides
            # whether the chip is switched into four byte addressing.
            if exponent > 40:
                return None
            bits = 1 << exponent
        else:
            bits = density + 1

        size = bits // 8
        if not MIN_PLAUSIBLE_SIZE <= size <= MAX_PLAUSIBLE_SIZE:
            return None
        if size & (size - 1):
            return None
        return size

    def status(self):
        return self.probe.xfer(bytes([CMD_READ_STATUS1]), rx_len=1)[0]

    def busy(self):
        return bool(self.status() & SR1_BUSY)

    def wait_ready(self, timeout_s=120.0, poll_s=0.0):
        deadline = time.monotonic() + timeout_s
        while self.busy():
            if time.monotonic() > deadline:
                raise NorError("timed out waiting for the chip to go ready")
            if poll_s:
                time.sleep(poll_s)

    def quad_enable(self):
        return bool(self.probe.xfer(bytes([CMD_READ_STATUS2]), rx_len=1)[0] & SR2_QE)

    def quad_disable(self):
        """Clear the quad enable bit so the chip cannot be put into QPI mode.

        QPI is entered with 0x38 and left only by clocking 0xFF on all four IO
        lines. A four wire probe cannot do that, so a chip that reaches QPI is
        stuck until it loses power. Any stray 0x38 interpreted as a command
        will do it, and random payload data is full of them. Clearing QE makes
        0x38 a no-op.

        The write is volatile: it reverts on the next power cycle and does not
        alter the chip's stored configuration.

        Not all parts accept it. The W25Q64JV here refuses via 0x50/0x31,
        0x06/0x31 and 0x01, so this is opt in and not relied upon.
        """
        sr2 = self.probe.xfer(bytes([CMD_READ_STATUS2]), rx_len=1)[0]
        if not sr2 & SR2_QE:
            return False

        self.probe.xfer(bytes([CMD_VOLATILE_SR_WRITE_ENABLE]))
        self.probe.xfer(bytes([CMD_WRITE_STATUS2, sr2 & ~SR2_QE]))

        if self.quad_enable():
            raise NorError("could not clear the quad enable bit")
        return True

    def protection(self):
        """Block protection state from SR1. `locked` means erase and program
        will silently fail for some or all of the chip."""
        sr1 = self.status()
        return {
            "sr1": sr1,
            "bp": (sr1 >> 2) & 0x07,
            "tb": bool(sr1 & 0x20),
            "sec": bool(sr1 & 0x40),
            "srp0": bool(sr1 & SR1_SRP0),
            "locked": bool(sr1 & SR1_PROTECT),
        }

    def unprotect(self):
        """Clear the block protection bits. SRP0 is left alone: setting it
        with WP# unconnected can make the status register unwritable."""
        sr1 = self.status()
        if not sr1 & SR1_PROTECT:
            return

        if sr1 & SR1_SRP0:
            raise NorError(
                "SRP0 is set, so the status register may be hardware protected. "
                "WP# is not wired on this probe, so the bits cannot be cleared."
            )

        sr2 = self.probe.xfer(bytes([CMD_READ_STATUS2]), rx_len=1)[0]
        self.probe.xfer_poll(bytes([CMD_WRITE_STATUS, sr1 & ~SR1_PROTECT, sr2]),
                             timeout_ms=ERASE_TIMEOUT_MS)

        if self.status() & SR1_PROTECT:
            raise NorError("block protection bits did not clear")

    def protect(self, bp):
        """Set the BP bits, for testing the protection path."""
        sr1 = self.status()
        sr2 = self.probe.xfer(bytes([CMD_READ_STATUS2]), rx_len=1)[0]
        new = (sr1 & ~SR1_PROTECT) | ((bp & 0x07) << 2)
        self.probe.xfer_poll(bytes([CMD_WRITE_STATUS, new, sr2]),
                             timeout_ms=ERASE_TIMEOUT_MS)

    def chip_erase(self):
        """Whole chip erase with C7h. Usually no faster than walking block
        opcodes on this part, and gives no progress, so erase() does not use
        it automatically."""
        self.probe.xfer_poll(bytes([CMD_CHIP_ERASE]), timeout_ms=CHIP_ERASE_TIMEOUT_MS)

    def write_enable(self):
        self.probe.xfer(bytes([CMD_WRITE_ENABLE]))
        if not self.status() & SR1_WEL:
            raise NorError("write enable latch did not set")

    def _read_chunk(self, addr, length):
        if self.fast_read:
            tx = bytes([CMD_FAST_READ]) + _addr_bytes(addr, self.addr_width) + b"\x00"
        else:
            tx = bytes([CMD_READ]) + _addr_bytes(addr, self.addr_width)
        return self.probe.xfer(tx, rx_len=length)

    def _read_tx(self, addr):
        if self.fast_read:
            return bytes([CMD_FAST_READ]) + _addr_bytes(addr, self.addr_width) + b"\x00"
        return bytes([CMD_READ]) + _addr_bytes(addr, self.addr_width)

    def read(self, addr, length, progress=None, depth=None):
        """Read with `depth` commands in flight so the SPI transfer of one
        chunk overlaps the USB transfer of the previous one."""
        from .transport import OP_SPI_XFER

        # Never queue more than the firmware can hold: it would block on a
        # reply buffer, stop draining requests, and deadlock against a host
        # that is still writing.
        limit = min(self.probe.in_depth, self.probe.out_depth)
        depth = limit if depth is None else min(depth, limit)

        overhead = 1 + self.addr_width + (1 if self.fast_read else 0)
        chunk = self.probe.max_xfer - overhead
        offsets = list(range(0, length, chunk))
        sizes = [min(chunk, length - o) for o in offsets]

        out = bytearray()
        inflight = []
        nxt = 0
        while len(out) < length:
            while len(inflight) < depth and nxt < len(offsets):
                tag = self.probe.submit(OP_SPI_XFER, self._read_tx(addr + offsets[nxt]),
                                        sizes[nxt])
                inflight.append((tag, sizes[nxt]))
                nxt += 1
            tag, size = inflight.pop(0)
            out += self.probe.collect(OP_SPI_XFER, tag, size)
            if progress:
                progress(len(out), length)
        return bytes(out)

    def _erase_step(self, addr, remaining):
        if addr % 65536 == 0 and remaining >= 65536:
            return CMD_BLOCK_ERASE_64K, 65536
        if addr % 32768 == 0 and remaining >= 32768:
            return CMD_BLOCK_ERASE_32K, 32768
        return CMD_SECTOR_ERASE_4K, 4096

    def erase(self, addr, length):
        if addr % 4096 or length % 4096:
            raise NorError("erase range must be 4 KiB aligned")
        end = addr + length
        while addr < end:
            opcode, step = self._erase_step(addr, end - addr)
            self.probe.xfer_poll(bytes([opcode]) + _addr_bytes(addr, self.addr_width),
                                 timeout_ms=ERASE_TIMEOUT_MS)
            addr += step

    def write(self, addr, data, progress=None, depth=None):
        """Program pre-erased pages. All-ones pages are skipped, so writing
        0xFF does not clear existing data; erase first."""
        from .transport import OP_XFER_POLL

        if addr % PAGE_SIZE:
            raise NorError("write address must be page aligned")

        limit = min(self.probe.in_depth, self.probe.out_depth)
        depth = limit if depth is None else min(depth, limit)

        blank = b"\xff" * PAGE_SIZE
        offsets = [o for o in range(0, len(data), PAGE_SIZE)
                   if data[o : o + PAGE_SIZE] != blank[: len(data) - o]]

        pending = []
        nxt = 0
        done = 0

        while nxt < len(offsets) or pending:
            while len(pending) < depth and nxt < len(offsets):
                off = offsets[nxt]
                frame = (bytes([CMD_PAGE_PROGRAM])
                         + _addr_bytes(addr + off, self.addr_width)
                         + data[off : off + PAGE_SIZE])
                args = self.probe.xfer_poll_args(frame, timeout_ms=PROGRAM_TIMEOUT_MS)
                pending.append(self.probe.submit(OP_XFER_POLL, args, 1))
                nxt += 1
            self.probe.collect(OP_XFER_POLL, pending.pop(0), 1)
            done += 1
            if progress:
                progress(done * PAGE_SIZE, len(offsets) * PAGE_SIZE)
