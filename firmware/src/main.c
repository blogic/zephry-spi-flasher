/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

#include "proto.h"
#include "spi_bus.h"
#include "usbd.h"

LOG_MODULE_REGISTER(main, CONFIG_LOG_DEFAULT_LEVEL);

#define STATUS_BLINK_PERIOD K_MSEC(250)

static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

static bool usb_configured;

static void status_tick(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	if (status_led.port == NULL) {
		return;
	}

	if (usb_configured) {
		gpio_pin_set_dt(&status_led, 1);
		return;
	}

	gpio_pin_toggle_dt(&status_led);
}

static K_TIMER_DEFINE(status_timer, status_tick, NULL);

static void status_init(void)
{
	if (status_led.port == NULL) {
		return;
	}

	if (!gpio_is_ready_dt(&status_led)) {
		LOG_WRN("Status LED not ready");
		return;
	}

	gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE);
	k_timer_start(&status_timer, STATUS_BLINK_PERIOD, STATUS_BLINK_PERIOD);
}

static void usb_message(struct usbd_context *ctx, const struct usbd_msg *msg)
{
	ARG_UNUSED(ctx);

	switch (msg->type) {
	case USBD_MSG_CONFIGURATION:
		usb_configured = true;
		break;
	case USBD_MSG_RESET:
	case USBD_MSG_SUSPEND:
	case USBD_MSG_VBUS_REMOVED:
		usb_configured = false;
		break;
	default:
		break;
	}
}

int main(void)
{
	int err;

	status_init();

	err = spi_bus_init();
	if (err) {
		LOG_ERR("SPI bus init failed (%d)", err);
		return err;
	}

	err = usb_device_start(usb_message);
	if (err) {
		return err;
	}

	LOG_INF("bulk probe ready, default SPI clock %u Hz", spi_bus_freq_get());

	proto_run();

	return 0;
}
