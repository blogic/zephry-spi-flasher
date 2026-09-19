/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_FLASHER_PROTO_H
#define SPI_FLASHER_PROTO_H

#include <stdint.h>

#include <zephyr/sys/util.h>

#define PROTO_VERSION 1

#define PROTO_REQ_HDR_LEN 12
#define PROTO_RSP_HDR_LEN 8

#define PROTO_OP_PING     0x00
#define PROTO_OP_SPI_XFER 0x01
#define PROTO_OP_TEST_IN  0x02
#define PROTO_OP_SET_FREQ 0x03
#define PROTO_OP_PIN_TEST 0x04
#define PROTO_OP_SPI_REGS 0x05
#define PROTO_OP_REBOOT   0x06
#define PROTO_OP_XFER_POLL 0x07
#define PROTO_OP_QPI_ESCAPE 0x08
#define PROTO_OP_BITBANG    0x09

#define PROTO_XFER_POLL_HDR_LEN 8
#define PROTO_FLAG_WRITE_ENABLE BIT(0)

#define PROTO_STATUS_OK          0x00
#define PROTO_STATUS_BAD_OPCODE  0x01
#define PROTO_STATUS_BAD_LENGTH  0x02
#define PROTO_STATUS_SHORT_REQ   0x03
#define PROTO_STATUS_SPI_ERROR   0x04
#define PROTO_STATUS_NO_RESOURCE 0x05
#define PROTO_STATUS_TIMEOUT     0x06

struct proto_request {
	uint8_t opcode;
	uint8_t flags;
	uint16_t tag;
	uint32_t tx_len;
	uint32_t rx_len;
};

void proto_run(void);

#endif
