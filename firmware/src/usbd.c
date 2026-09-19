/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usbd.h"

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_device, CONFIG_LOG_DEFAULT_LEVEL);

USBD_DEVICE_DEFINE(spi_flasher_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SPI_FLASHER_USB_VID, CONFIG_SPI_FLASHER_USB_PID);

USBD_DESC_LANG_DEFINE(spi_flasher_lang);
USBD_DESC_MANUFACTURER_DEFINE(spi_flasher_mfr, CONFIG_SPI_FLASHER_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(spi_flasher_product, CONFIG_SPI_FLASHER_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(spi_flasher_sn);
USBD_DESC_CONFIG_DEFINE(spi_flasher_fs_desc, "Full speed configuration");

USBD_CONFIGURATION_DEFINE(spi_flasher_fs_config, 0,
			  CONFIG_SPI_FLASHER_USB_MAX_POWER, &spi_flasher_fs_desc);

static const char *const spi_flasher_blocklist[] = {
	"dfu_dfu",
	NULL,
};

int usb_device_start(usbd_msg_cb_t msg_cb)
{
	int err;

	err = usbd_add_descriptor(&spi_flasher_usbd, &spi_flasher_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&spi_flasher_usbd, &spi_flasher_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&spi_flasher_usbd, &spi_flasher_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&spi_flasher_usbd, &spi_flasher_sn);
	if (err) {
		LOG_ERR("Failed to add serial number descriptor (%d)", err);
		return err;
	}

	err = usbd_add_configuration(&spi_flasher_usbd, USBD_SPEED_FS,
				     &spi_flasher_fs_config);
	if (err) {
		LOG_ERR("Failed to add full speed configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&spi_flasher_usbd, USBD_SPEED_FS, 1,
					spi_flasher_blocklist);
	if (err) {
		LOG_ERR("Failed to register classes (%d)", err);
		return err;
	}

	usbd_device_set_code_triple(&spi_flasher_usbd, USBD_SPEED_FS, 0, 0, 0);

	if (msg_cb != NULL) {
		err = usbd_msg_register_cb(&spi_flasher_usbd, msg_cb);
		if (err) {
			LOG_ERR("Failed to register message callback (%d)", err);
			return err;
		}
	}

	err = usbd_init(&spi_flasher_usbd);
	if (err) {
		LOG_ERR("Failed to initialise device support (%d)", err);
		return err;
	}

	err = usbd_enable(&spi_flasher_usbd);
	if (err) {
		LOG_ERR("Failed to enable device support (%d)", err);
		return err;
	}

	return 0;
}
