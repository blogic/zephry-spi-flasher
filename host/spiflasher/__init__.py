# Copyright (c) 2026 John Crispin <john@phrozen.org>
#
# SPDX-License-Identifier: Apache-2.0

import sys
import time

import usb.core

from .transport import PID, VID, Probe, ProbeError

__all__ = ["Probe", "ProbeError", "connect", "open_nor"]


def connect(timeout=20.0):
    """Open the probe, waiting for it to appear on the bus."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        if usb.core.find(idVendor=VID, idProduct=PID) is not None:
            try:
                return Probe()
            except Exception as err:
                last = err
        time.sleep(0.2)
    raise ProbeError(f"no probe appeared within {timeout:.0f} s ({last})")


def open_nor(probe=None, retries=1, **kw):
    """Attach to the SPI-NOR chip, warm resetting the probe if the bus comes
    up dead. The chip intermittently stops answering, cause unresolved; a
    probe reset has so far always cleared it."""
    from .nor import NorError, SpiNor

    probe = probe or connect()
    for attempt in range(retries + 1):
        try:
            return probe, SpiNor(probe, **kw)
        except NorError as err:
            if attempt == retries:
                raise
            print(f"warning: {err}", file=sys.stderr)
            print("warning: warm resetting the probe and retrying", file=sys.stderr)
            probe.reboot()
            time.sleep(1.0)
            probe = connect()
