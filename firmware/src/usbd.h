/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_FLASHER_USBD_H
#define SPI_FLASHER_USBD_H

#include <zephyr/usb/usbd.h>

int usb_device_start(usbd_msg_cb_t msg_cb);

#endif
