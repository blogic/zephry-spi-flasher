# Copyright (c) 2026 John Crispin <john@phrozen.org>
#
# SPDX-License-Identifier: Apache-2.0

"""libusb transport for the spi-flasher probe's vendor bulk interface."""

import struct

import usb.core
import usb.util

VID = 0x2FE3
PID = 0x0001

EP_OUT = 0x01
EP_IN = 0x81

REQ_HDR = struct.Struct("<BBHII")
RSP_HDR = struct.Struct("<BBHI")

OP_PING = 0x00
OP_SPI_XFER = 0x01
OP_TEST_IN = 0x02
OP_SET_FREQ = 0x03
OP_PIN_TEST = 0x04
OP_SPI_REGS = 0x05
OP_REBOOT = 0x06
OP_XFER_POLL = 0x07
OP_QPI_ESCAPE = 0x08
OP_BITBANG = 0x09

FLAG_WRITE_ENABLE = 1 << 0

STATUS = {
    0x00: "ok",
    0x01: "bad opcode",
    0x02: "bad length",
    0x03: "short request",
    0x04: "spi error",
    0x05: "no resource",
    0x06: "timed out waiting for the chip",
}


class ProbeError(Exception):
    pass


class Probe:
    def __init__(self, vid=VID, pid=PID, timeout_ms=5000):
        self.timeout_ms = timeout_ms
        self.dev = usb.core.find(idVendor=vid, idProduct=pid)
        if self.dev is None:
            raise ProbeError(f"no probe with {vid:04x}:{pid:04x} on the bus")

        self.dev.set_configuration()
        self._tag = 0
        self.version, self.max_xfer, self.frequency = self.ping()

    def close(self):
        usb.util.dispose_resources(self.dev)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def _next_tag(self):
        self._tag = (self._tag + 1) & 0xFFFF
        return self._tag

    def submit(self, opcode, tx=b"", rx_len=0, flags=0):
        """Queue a command without waiting for its reply. The firmware keeps
        several transfers in flight, so replies can be collected later."""
        tag = self._next_tag()
        self.dev.write(EP_OUT, REQ_HDR.pack(opcode, flags, tag, len(tx), rx_len) + tx,
                       self.timeout_ms)
        return tag

    def collect(self, opcode, tag, rx_len):
        want = RSP_HDR.size + rx_len
        reply = self.dev.read(EP_IN, want, self.timeout_ms)
        if len(reply) < RSP_HDR.size:
            raise ProbeError(f"runt reply of {len(reply)} bytes")

        status, op, got_tag, got_len = RSP_HDR.unpack(bytes(reply[: RSP_HDR.size]))
        if status:
            raise ProbeError(f"opcode 0x{opcode:02x}: {STATUS.get(status, status)}")
        if got_tag != tag:
            raise ProbeError(f"tag mismatch, sent {tag} got {got_tag}")
        if op != opcode:
            raise ProbeError(f"opcode mismatch, sent 0x{opcode:02x} got 0x{op:02x}")

        return bytes(reply[RSP_HDR.size : RSP_HDR.size + got_len])

    def command(self, opcode, tx=b"", rx_len=0, flags=0):
        tag = self.submit(opcode, tx, rx_len, flags)
        return self.collect(opcode, tag, rx_len)

    def ping(self):
        out = self.command(OP_PING, rx_len=16)
        (version, max_xfer, frequency, self.last_spi_err,
         self.in_depth, self.out_depth) = struct.unpack("<HIIiBB", out)
        return version, max_xfer, frequency

    def test_in(self, length):
        return self.command(OP_TEST_IN, rx_len=length)

    def xfer(self, tx=b"", rx_len=0):
        """One chip-select frame: clock out tx, then read rx_len bytes."""
        return self.command(OP_SPI_XFER, tx=tx, rx_len=rx_len)

    def pin_test(self):
        """Per SPI pin: (level with pull-up, level with pull-down).
        (1, 0) means nothing is driving it. Equal values mean something is."""
        out = self.command(OP_PIN_TEST, rx_len=8)
        names = ("CS/PB12", "SCK/PB13", "MISO/PB14", "MOSI/PB15")
        return dict(zip(names, [(out[i], out[i + 1]) for i in range(0, 8, 2)]))

    def reboot(self):
        """Warm reset the probe. The device drops off the bus; reopen after."""
        try:
            self.command(OP_REBOOT)
        except Exception:
            pass
        self.close()

    def spi_regs(self):
        (cr1, cr2, sr, moder, afrh,
         pupdr, odr, idr) = struct.unpack("<8I", self.command(OP_SPI_REGS, rx_len=32))
        flags = {"RXNE": 1 << 0, "TXE": 1 << 1, "CRCERR": 1 << 4, "MODF": 1 << 5,
                 "OVR": 1 << 6, "BSY": 1 << 7, "FRE": 1 << 8}
        modes = ("input", "output", "altfunc", "analog")
        pins = {}
        for pin in (12, 13, 14, 15):
            pins[f"PB{pin}"] = {
                "mode": modes[(moder >> (pin * 2)) & 3],
                "af": (afrh >> ((pin - 8) * 4)) & 0xF,
                "pull": ("none", "up", "down", "?")[(pupdr >> (pin * 2)) & 3],
                "out": (odr >> pin) & 1,
                "in": (idr >> pin) & 1,
            }
        return {"CR1": cr1, "CR2": cr2, "SR": sr, "MODER": moder, "AFRH": afrh,
                "set": [n for n, m in flags.items() if sr & m], "pins": pins}

    def bitbang(self, tx=b"", rx_len=0):
        """One CS frame driven from GPIO, bypassing the SPI peripheral, its
        DMA and the driver's chip select handling."""
        return self.command(OP_BITBANG, tx=tx, rx_len=rx_len)

    def qpi_escape(self):
        """Clock 0xFF on the QPI data lines to leave QPI mode. Only works if
        IO2 and IO3 are pulled high on the breakout, so check the result."""
        self.command(OP_QPI_ESCAPE)

    def xfer_poll_args(self, frame, poll_cmd=b"\x05", mask=0x01, value=0x00,
                       timeout_ms=3000, write_enable=True):
        flags = FLAG_WRITE_ENABLE if write_enable else 0
        return (struct.pack("<BBBBHH", flags, len(poll_cmd), mask, value,
                            timeout_ms, len(frame)) + poll_cmd + frame)

    def xfer_poll(self, frame, **kw):
        """One CS frame, optionally preceded by write enable, then poll the
        status register on the device until ready. One round trip per page."""
        return self.command(OP_XFER_POLL, tx=self.xfer_poll_args(frame, **kw), rx_len=1)[0]

    def frequency_set(self, hz):
        out = self.command(OP_SET_FREQ, tx=struct.pack("<I", hz), rx_len=4)
        self.frequency = struct.unpack("<I", out)[0]
        return self.frequency
