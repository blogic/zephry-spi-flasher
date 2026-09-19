# spi-flasher

A USB probe for reading, erasing and programming SPI flash.

The firmware is Zephyr on a WeAct Studio STM32G474 core board. The host side is
Python over libusb. The probe presents a vendor bulk interface rather than a
serial port, so several commands can be in flight at once and programming a
page costs one USB round trip instead of four.

On a W25Q64JV it reads 8 MB in about 11 seconds and writes it in about 25.

Apache-2.0, same as Zephyr. See `LICENSE`.

## Wiring

Six wires. The four signals sit in a 2x2 block on header P1, with ground on the
row above.

```
P1   1  2    VCC   VCC     <- regulator input, ~4.7 V. Do not use.
     3  4    GND   GND
     5  6    PB12  PB13    CS    CLK
     7  8    PB14  PB15    DO    DI
```

| Flash pin | Board |
|---|---|
| CS | PB12, P1 pin 5 |
| CLK | PB13, P1 pin 6 |
| DO | PB14, P1 pin 7 |
| DI | PB15, P1 pin 8 |
| GND | P1 pin 3 or 4 |
| VCC | 3V3 on **P2** pin 1 or 2 |

Take 3.3 V from P2, never from P1. The pins marked VCC on P1 are the input side
of the regulator and sit near 4.7 V on USB power, which will destroy a 3.3 V
part.

If your breakout does not expose WP# and HOLD#, check that it pulls them high
on-board. A floating WP# blocks writes and a floating HOLD# stalls transfers
part way through.

There is a serial console on USART1, PA9 and PA10, at 115200. Without an
adapter the blue LED is the only sign of life: it blinks until the host
configures USB, then stays on.

## Getting set up

The directory is a west workspace. Everything west manages is git-ignored.

```sh
git clone --depth 1 -b v4.4.2 https://github.com/zephyrproject-rtos/zephyr zephyr
west init -l zephyr
west update --narrow -o=--depth=1
python3 -m venv .venv
.venv/bin/pip install -r zephyr/scripts/requirements.txt pyusb
```

You need Zephyr SDK 1.0.1, to match `zephyr/SDK_VERSION`.

## Build and flash

```sh
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
.venv/bin/west build -p -b weact_stm32g474_core firmware -d build
```

Flashing goes through the STM32 bootloader over USB, so no debug probe is
needed. Hold BOOT0, tap NRST, let go of BOOT0, then:

```sh
.venv/bin/west flash -d build
```

`west flash` always ends with a `get_status` error. That is the bootloader
jumping straight to the application without replying; look for `File downloaded
successfully` just above it.

## Using it

```python
from spiflasher import open_nor

probe, flash = open_nor()
print(bytes(flash.jedec_id).hex(), flash.size)

image = flash.read(0, flash.size)
flash.erase(0, flash.size)
flash.write(0, image)
```

`host/selftest.py` exercises the lot against a scratch chip and is destructive.
`host/bench.py` measures the USB path on its own, without touching SPI.

## What is where

```
boards/weact/stm32g474_core/   board definition, not in Zephyr upstream
firmware/src/usb_bulk.c        USB vendor class, bulk in and out
firmware/src/proto.c           command dispatch
firmware/src/spi_bus.c         one chip select frame per call
host/spiflasher/transport.py   libusb transport
host/spiflasher/nor.py         SPI-NOR
```

## Licensing

Everything here is Apache-2.0. The firmware also links Zephyr and CMSIS
(Apache-2.0), ST's HAL and LL (BSD-3-Clause), and picolibc and libgcc from the
Zephyr SDK (BSD variants, and GPLv3 with the Runtime Library Exception).

Apache-2.0 cannot be combined with GPL-2.0-only, so nothing here may absorb
GPL-2.0-only code. SNANDer is GPL-2.0-or-later and none of it is used: the host
is a clean-room implementation written from the chip datasheets.
