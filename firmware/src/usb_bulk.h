/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_FLASHER_USB_BULK_H
#define SPI_FLASHER_USB_BULK_H

#include <stddef.h>

#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>

struct net_buf *usb_bulk_rx_get(k_timeout_t timeout);
void usb_bulk_rx_put(struct net_buf *buf);

struct net_buf *usb_bulk_tx_alloc(size_t len, k_timeout_t timeout);
int usb_bulk_tx_submit(struct net_buf *buf);

#endif
