/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "proto.h"

#include <string.h>

#include <soc.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include "spi_bus.h"
#include "usb_bulk.h"

LOG_MODULE_REGISTER(proto, CONFIG_LOG_DEFAULT_LEVEL);

#define PROTO_PING_RSP_LEN 16

#define PROTO_SPI_NODE DT_NODELABEL(spi2)

PINCTRL_DT_DEV_CONFIG_DECLARE(PROTO_SPI_NODE);

static const struct device *const proto_gpiob = DEVICE_DT_GET(DT_NODELABEL(gpiob));
static const uint8_t proto_test_pins[] = { 12, 13, 14, 15 };
#define PROTO_PIN_TEST_RSP_LEN (2 * ARRAY_SIZE(proto_test_pins))
#define PROTO_SPI_REGS_RSP_LEN 32

static void proto_spi_pins_restore(void);

static uint8_t proto_tx_buf[CONFIG_SPI_FLASHER_XFER_MAX];
static uint8_t proto_rx_buf[CONFIG_SPI_FLASHER_XFER_MAX];
static int32_t proto_last_spi_err;

static void proto_parse(const uint8_t *raw, struct proto_request *req)
{
	req->opcode = raw[0];
	req->flags = raw[1];
	req->tag = sys_get_le16(&raw[2]);
	req->tx_len = sys_get_le32(&raw[4]);
	req->rx_len = sys_get_le32(&raw[8]);
}

static struct net_buf *proto_reply(const struct proto_request *req, uint8_t status,
				   uint32_t rx_len)
{
	struct net_buf *buf;
	uint8_t *hdr;

	buf = usb_bulk_tx_alloc(PROTO_RSP_HDR_LEN + rx_len, K_FOREVER);
	if (buf == NULL) {
		return NULL;
	}

	hdr = net_buf_add(buf, PROTO_RSP_HDR_LEN);
	hdr[0] = status;
	hdr[1] = req->opcode;
	sys_put_le16(req->tag, &hdr[2]);
	sys_put_le32(rx_len, &hdr[4]);

	return buf;
}

static void proto_fail(const struct proto_request *req, uint8_t status)
{
	struct net_buf *buf = proto_reply(req, status, 0);

	if (buf != NULL) {
		usb_bulk_tx_submit(buf);
	}
}

static void proto_handle_ping(const struct proto_request *req)
{
	struct net_buf *buf = proto_reply(req, PROTO_STATUS_OK, PROTO_PING_RSP_LEN);
	uint8_t *out;

	if (buf == NULL) {
		return;
	}

	out = net_buf_add(buf, PROTO_PING_RSP_LEN);
	sys_put_le16(PROTO_VERSION, &out[0]);
	sys_put_le32(CONFIG_SPI_FLASHER_XFER_MAX, &out[2]);
	sys_put_le32(spi_bus_freq_get(), &out[6]);
	sys_put_le32((uint32_t)proto_last_spi_err, &out[10]);
	out[14] = CONFIG_SPI_FLASHER_USB_IN_DEPTH;
	out[15] = CONFIG_SPI_FLASHER_USB_OUT_DEPTH;

	usb_bulk_tx_submit(buf);
}

static void proto_handle_test_in(const struct proto_request *req)
{
	struct net_buf *buf = proto_reply(req, PROTO_STATUS_OK, req->rx_len);
	uint8_t *out;

	if (buf == NULL) {
		return;
	}

	out = net_buf_add(buf, req->rx_len);
	for (uint32_t i = 0; i < req->rx_len; i++) {
		out[i] = (uint8_t)i;
	}

	usb_bulk_tx_submit(buf);
}

static void proto_handle_spi_xfer(const struct proto_request *req, const uint8_t *tx)
{
	struct net_buf *buf;
	uint8_t *out;
	int err;

	memcpy(proto_tx_buf, tx, req->tx_len);

	err = spi_bus_xfer(proto_tx_buf, req->tx_len, proto_rx_buf, req->rx_len);
	proto_last_spi_err = err;

	buf = proto_reply(req, err ? PROTO_STATUS_SPI_ERROR : PROTO_STATUS_OK,
			  err ? 0 : req->rx_len);
	if (buf == NULL) {
		return;
	}

	if (!err) {
		out = net_buf_add(buf, req->rx_len);
		memcpy(out, proto_rx_buf, req->rx_len);
	}

	usb_bulk_tx_submit(buf);
}

static void proto_handle_set_freq(const struct proto_request *req, const uint8_t *tx)
{
	struct net_buf *buf;
	uint32_t actual;

	if (req->tx_len != sizeof(uint32_t)) {
		proto_fail(req, PROTO_STATUS_BAD_LENGTH);
		return;
	}

	actual = spi_bus_freq_set(sys_get_le32(tx));
	if (actual == 0) {
		proto_fail(req, PROTO_STATUS_BAD_LENGTH);
		return;
	}

	buf = proto_reply(req, PROTO_STATUS_OK, sizeof(uint32_t));
	if (buf == NULL) {
		return;
	}

	sys_put_le32(actual, net_buf_add(buf, sizeof(uint32_t)));
	usb_bulk_tx_submit(buf);
}

/* Read each SPI pin as a plain input, once biased high and once biased low.
 * A line nothing is driving follows the bias and reads 1 then 0. A line held
 * by something else reads the same value both ways.
 */
static void proto_handle_pin_test(const struct proto_request *req)
{
	struct net_buf *buf = proto_reply(req, PROTO_STATUS_OK, PROTO_PIN_TEST_RSP_LEN);
	uint8_t *out;

	if (buf == NULL) {
		return;
	}

	out = net_buf_add(buf, PROTO_PIN_TEST_RSP_LEN);

	for (size_t i = 0; i < ARRAY_SIZE(proto_test_pins); i++) {
		uint8_t pin = proto_test_pins[i];

		gpio_pin_configure(proto_gpiob, pin, GPIO_INPUT | GPIO_PULL_UP);
		k_busy_wait(50);
		out[2 * i] = gpio_pin_get_raw(proto_gpiob, pin);

		gpio_pin_configure(proto_gpiob, pin, GPIO_INPUT | GPIO_PULL_DOWN);
		k_busy_wait(50);
		out[2 * i + 1] = gpio_pin_get_raw(proto_gpiob, pin);
	}

	proto_spi_pins_restore();

	usb_bulk_tx_submit(buf);
}

/* Raw peripheral state, so a wedged bus can be observed rather than inferred.
 * On this IP the overrun flag latches: once set the data register stops
 * updating and every read returns stale data until it is cleared.
 */
#define QPI_PIN_CS   12
#define QPI_PIN_SCK  13
#define QPI_PIN_IO1  14
#define QPI_PIN_IO0  15
#define QPI_ESCAPE_CLOCKS 8

/* Chip select is active low. The GPIO driver keeps that inversion per pin at
 * configure time, so restoring the pin without the flag would leave the SPI
 * driver driving chip select the wrong way round and the chip never selected.
 */
static void proto_spi_pins_restore(void)
{
	pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(PROTO_SPI_NODE), PINCTRL_STATE_DEFAULT);
	gpio_pin_configure(proto_gpiob, 12, GPIO_OUTPUT_INACTIVE | GPIO_ACTIVE_LOW);
}

/* Mode 0 transfer driven entirely from GPIO, bypassing the SPI peripheral,
 * its DMA and the driver's chip select handling. Used to decide whether a
 * silent bus is the chip or everything between here and it.
 */
static void proto_handle_bitbang(const struct proto_request *req, const uint8_t *tx)
{
	struct net_buf *buf;
	uint8_t *out;

	if (req->tx_len > sizeof(proto_tx_buf) || req->rx_len > sizeof(proto_rx_buf)) {
		proto_fail(req, PROTO_STATUS_BAD_LENGTH);
		return;
	}

	memcpy(proto_tx_buf, tx, req->tx_len);

	gpio_pin_configure(proto_gpiob, QPI_PIN_CS, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(proto_gpiob, QPI_PIN_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(proto_gpiob, QPI_PIN_IO0, GPIO_OUTPUT_LOW);
	gpio_pin_configure(proto_gpiob, QPI_PIN_IO1, GPIO_INPUT);
	k_busy_wait(10);

	gpio_pin_set_raw(proto_gpiob, QPI_PIN_CS, 0);
	k_busy_wait(5);

	for (uint32_t i = 0; i < req->tx_len + req->rx_len; i++) {
		uint8_t value = i < req->tx_len ? proto_tx_buf[i] : 0x00;
		uint8_t got = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_pin_set_raw(proto_gpiob, QPI_PIN_IO0, (value >> bit) & 1);
			k_busy_wait(2);
			gpio_pin_set_raw(proto_gpiob, QPI_PIN_SCK, 1);
			k_busy_wait(2);
			got = (got << 1) | gpio_pin_get_raw(proto_gpiob, QPI_PIN_IO1);
			gpio_pin_set_raw(proto_gpiob, QPI_PIN_SCK, 0);
			k_busy_wait(2);
		}

		if (i >= req->tx_len) {
			proto_rx_buf[i - req->tx_len] = got;
		}
	}

	gpio_pin_set_raw(proto_gpiob, QPI_PIN_CS, 1);
	k_busy_wait(10);
	proto_spi_pins_restore();

	buf = proto_reply(req, PROTO_STATUS_OK, req->rx_len);
	if (buf == NULL) {
		return;
	}

	out = net_buf_add(buf, req->rx_len);
	memcpy(out, proto_rx_buf, req->rx_len);
	usb_bulk_tx_submit(buf);
}

static void proto_handle_spi_regs(const struct proto_request *req)
{
	SPI_TypeDef *spi = (SPI_TypeDef *)DT_REG_ADDR(PROTO_SPI_NODE);
	struct net_buf *buf = proto_reply(req, PROTO_STATUS_OK, PROTO_SPI_REGS_RSP_LEN);
	uint8_t *out;

	if (buf == NULL) {
		return;
	}

	GPIO_TypeDef *gpiob = (GPIO_TypeDef *)DT_REG_ADDR(DT_NODELABEL(gpiob));

	out = net_buf_add(buf, PROTO_SPI_REGS_RSP_LEN);
	sys_put_le32(spi->CR1, &out[0]);
	sys_put_le32(spi->CR2, &out[4]);
	sys_put_le32(spi->SR, &out[8]);
	sys_put_le32(gpiob->MODER, &out[12]);
	sys_put_le32(gpiob->AFR[1], &out[16]);
	sys_put_le32(gpiob->PUPDR, &out[20]);
	sys_put_le32(gpiob->ODR, &out[24]);
	sys_put_le32(gpiob->IDR, &out[28]);

	usb_bulk_tx_submit(buf);
}

/* Warm reset, so boot behaviour can be exercised without touching the board. */
static void proto_handle_reboot(const struct proto_request *req)
{
	struct net_buf *buf = proto_reply(req, PROTO_STATUS_OK, 0);

	if (buf != NULL) {
		usb_bulk_tx_submit(buf);
	}

	k_sleep(K_MSEC(50));
	sys_reboot(SYS_REBOOT_WARM);
}

/* One chip-select frame, optionally preceded by a write enable, then poll a
 * status register until the chip reports ready. Collapses what would be four
 * or more USB round trips per page into one.
 *
 * Payload: flags, poll_len, poll_mask, poll_value, timeout_ms (16),
 * frame_len (16), then poll_len bytes of poll command, then the frame.
 */
static void proto_handle_xfer_poll(const struct proto_request *req, const uint8_t *tx)
{
	static const uint8_t write_enable = 0x06;
	const uint8_t *poll_cmd, *frame;
	uint8_t flags, poll_len, poll_mask, poll_value, status = 0;
	uint16_t timeout_ms, frame_len;
	struct net_buf *buf;
	int64_t deadline;
	bool ready = false;

	if (req->tx_len < PROTO_XFER_POLL_HDR_LEN) {
		proto_fail(req, PROTO_STATUS_SHORT_REQ);
		return;
	}

	flags = tx[0];
	poll_len = tx[1];
	poll_mask = tx[2];
	poll_value = tx[3];
	timeout_ms = sys_get_le16(&tx[4]);
	frame_len = sys_get_le16(&tx[6]);

	if (req->tx_len != PROTO_XFER_POLL_HDR_LEN + poll_len + frame_len || poll_len == 0) {
		proto_fail(req, PROTO_STATUS_BAD_LENGTH);
		return;
	}

	poll_cmd = &tx[PROTO_XFER_POLL_HDR_LEN];
	frame = poll_cmd + poll_len;

	memcpy(proto_tx_buf, frame, frame_len);

	if (flags & PROTO_FLAG_WRITE_ENABLE) {
		if (spi_bus_xfer(&write_enable, 1, NULL, 0) < 0) {
			proto_fail(req, PROTO_STATUS_SPI_ERROR);
			return;
		}
	}

	if (spi_bus_xfer(proto_tx_buf, frame_len, NULL, 0) < 0) {
		proto_fail(req, PROTO_STATUS_SPI_ERROR);
		return;
	}

	deadline = k_uptime_get() + timeout_ms;
	do {
		if (spi_bus_xfer(poll_cmd, poll_len, &status, 1) < 0) {
			proto_fail(req, PROTO_STATUS_SPI_ERROR);
			return;
		}
		if ((status & poll_mask) == poll_value) {
			ready = true;
			break;
		}
	} while (k_uptime_get() < deadline);

	buf = proto_reply(req, ready ? PROTO_STATUS_OK : PROTO_STATUS_TIMEOUT, 1);
	if (buf == NULL) {
		return;
	}

	*(uint8_t *)net_buf_add(buf, 1) = status;
	usb_bulk_tx_submit(buf);
}

/* Clock 0xFF on all four QPI data lines to leave QPI mode.
 *
 * A chip in QPI ignores single line commands, so the normal reset sequences
 * cannot reach it and only a power cycle clears it. This wiring brings out
 * IO0 and IO1 only; IO2 and IO3 are left to the pull ups on the breakout. It
 * therefore works only if those really are pulled high, which is why the
 * result is verified by the caller rather than assumed here.
 */

static void proto_handle_qpi_escape(const struct proto_request *req)
{
	struct net_buf *buf;

	gpio_pin_configure(proto_gpiob, QPI_PIN_CS, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(proto_gpiob, QPI_PIN_SCK, GPIO_OUTPUT_LOW);
	gpio_pin_configure(proto_gpiob, QPI_PIN_IO0, GPIO_OUTPUT_HIGH);
	gpio_pin_configure(proto_gpiob, QPI_PIN_IO1, GPIO_OUTPUT_HIGH);
	k_busy_wait(10);

	gpio_pin_set_raw(proto_gpiob, QPI_PIN_CS, 0);
	k_busy_wait(2);

	for (int i = 0; i < QPI_ESCAPE_CLOCKS; i++) {
		gpio_pin_set_raw(proto_gpiob, QPI_PIN_SCK, 1);
		k_busy_wait(2);
		gpio_pin_set_raw(proto_gpiob, QPI_PIN_SCK, 0);
		k_busy_wait(2);
	}

	gpio_pin_set_raw(proto_gpiob, QPI_PIN_CS, 1);
	k_busy_wait(10);

	gpio_pin_configure(proto_gpiob, QPI_PIN_IO1, GPIO_INPUT);
	proto_spi_pins_restore();

	buf = proto_reply(req, PROTO_STATUS_OK, 0);
	if (buf != NULL) {
		usb_bulk_tx_submit(buf);
	}
}

static void proto_dispatch(struct net_buf *raw)
{
	struct proto_request req;
	const uint8_t *tx;

	if (raw->len < PROTO_REQ_HDR_LEN) {
		LOG_ERR("Runt request of %u bytes", raw->len);
		return;
	}

	proto_parse(raw->data, &req);
	tx = raw->data + PROTO_REQ_HDR_LEN;

	if (req.tx_len > CONFIG_SPI_FLASHER_XFER_MAX ||
	    req.rx_len > CONFIG_SPI_FLASHER_XFER_MAX) {
		proto_fail(&req, PROTO_STATUS_BAD_LENGTH);
		return;
	}

	if (raw->len != PROTO_REQ_HDR_LEN + req.tx_len) {
		proto_fail(&req, PROTO_STATUS_SHORT_REQ);
		return;
	}

	switch (req.opcode) {
	case PROTO_OP_PING:
		proto_handle_ping(&req);
		return;
	case PROTO_OP_SPI_XFER:
		proto_handle_spi_xfer(&req, tx);
		return;
	case PROTO_OP_TEST_IN:
		proto_handle_test_in(&req);
		return;
	case PROTO_OP_SET_FREQ:
		proto_handle_set_freq(&req, tx);
		return;
	case PROTO_OP_PIN_TEST:
		proto_handle_pin_test(&req);
		return;
	case PROTO_OP_SPI_REGS:
		proto_handle_spi_regs(&req);
		return;
	case PROTO_OP_REBOOT:
		proto_handle_reboot(&req);
		return;
	case PROTO_OP_XFER_POLL:
		proto_handle_xfer_poll(&req, tx);
		return;
	case PROTO_OP_QPI_ESCAPE:
		proto_handle_qpi_escape(&req);
		return;
	case PROTO_OP_BITBANG:
		proto_handle_bitbang(&req, tx);
		return;
	default:
		proto_fail(&req, PROTO_STATUS_BAD_OPCODE);
		return;
	}
}

void proto_run(void)
{
	while (true) {
		struct net_buf *raw = usb_bulk_rx_get(K_FOREVER);

		proto_dispatch(raw);
		usb_bulk_rx_put(raw);
	}
}
