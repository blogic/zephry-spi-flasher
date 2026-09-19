/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb_bulk.h"
#include "proto.h"

#include <zephyr/drivers/usb/udc.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/usb/usbd.h>

LOG_MODULE_REGISTER(usb_bulk, CONFIG_LOG_DEFAULT_LEVEL);

#define USB_BULK_EP_OUT 0x01
#define USB_BULK_EP_IN  0x81

#define USB_BULK_MPS_FS 64
#define USB_BULK_MPS_HS 512

#define USB_BULK_OUT_LEN (PROTO_REQ_HDR_LEN + CONFIG_SPI_FLASHER_XFER_MAX)
#define USB_BULK_IN_LEN  (PROTO_RSP_HDR_LEN + CONFIG_SPI_FLASHER_XFER_MAX)

#define USB_BULK_STATE_ENABLED 0

struct usb_bulk_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_out_ep;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_hs_out_ep;
	struct usb_ep_descriptor if0_hs_in_ep;
	struct usb_desc_header nil_desc;
};

struct usb_bulk_data {
	struct usb_bulk_desc *const desc;
	const struct usb_desc_header **const fs_desc;
	const struct usb_desc_header **const hs_desc;
	struct usbd_class_data *c_data;
	atomic_t state;
};

/* A buffer is the flow control credit. Exactly OUT_DEPTH request buffers and
 * IN_DEPTH reply buffers exist, so allocation can never fail and a reply can
 * never be dropped. A request buffer is only re-armed once the handler has
 * released it, which bounds the receive backlog.
 */
UDC_BUF_POOL_DEFINE(usb_bulk_pool,
		    CONFIG_SPI_FLASHER_USB_OUT_DEPTH + CONFIG_SPI_FLASHER_USB_IN_DEPTH,
		    USB_BULK_OUT_LEN, sizeof(struct udc_buf_info), NULL);

static struct net_buf *usb_bulk_buf_alloc(uint8_t ep)
{
	struct net_buf *buf = net_buf_alloc(&usb_bulk_pool, K_NO_WAIT);
	struct udc_buf_info *bi;

	if (buf == NULL) {
		return NULL;
	}

	bi = udc_get_buf_info(buf);
	bi->ep = ep;

	return buf;
}

static K_FIFO_DEFINE(usb_bulk_rx_fifo);
static K_SEM_DEFINE(usb_bulk_tx_sem, CONFIG_SPI_FLASHER_USB_IN_DEPTH,
		    CONFIG_SPI_FLASHER_USB_IN_DEPTH);

static struct usb_bulk_desc usb_bulk_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = USB_BULK_EP_OUT,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(USB_BULK_MPS_FS),
		.bInterval = 0,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = USB_BULK_EP_IN,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(USB_BULK_MPS_FS),
		.bInterval = 0,
	},
	.if0_hs_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = USB_BULK_EP_OUT,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(USB_BULK_MPS_HS),
		.bInterval = 0,
	},
	.if0_hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = USB_BULK_EP_IN,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(USB_BULK_MPS_HS),
		.bInterval = 0,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *usb_bulk_fs_desc[] = {
	(struct usb_desc_header *)&usb_bulk_desc.if0,
	(struct usb_desc_header *)&usb_bulk_desc.if0_out_ep,
	(struct usb_desc_header *)&usb_bulk_desc.if0_in_ep,
	(struct usb_desc_header *)&usb_bulk_desc.nil_desc,
};

static const struct usb_desc_header *usb_bulk_hs_desc[] = {
	(struct usb_desc_header *)&usb_bulk_desc.if0,
	(struct usb_desc_header *)&usb_bulk_desc.if0_hs_out_ep,
	(struct usb_desc_header *)&usb_bulk_desc.if0_hs_in_ep,
	(struct usb_desc_header *)&usb_bulk_desc.nil_desc,
};

static struct usb_bulk_data usb_bulk_data = {
	.desc = &usb_bulk_desc,
	.fs_desc = usb_bulk_fs_desc,
	.hs_desc = usb_bulk_hs_desc,
};

static int usb_bulk_submit_out(struct usbd_class_data *const c_data)
{
	struct usb_bulk_data *data = usbd_class_get_private(c_data);
	struct net_buf *buf;
	int err;

	if (!atomic_test_bit(&data->state, USB_BULK_STATE_ENABLED)) {
		return -EPERM;
	}

	buf = usb_bulk_buf_alloc(USB_BULK_EP_OUT);
	if (buf == NULL) {
		LOG_ERR("No buffer for bulk OUT");
		return -ENOMEM;
	}

	err = usbd_ep_enqueue(c_data, buf);
	if (err) {
		LOG_ERR("Failed to enqueue bulk OUT (%d)", err);
		net_buf_unref(buf);
	}

	return err;
}

static int usb_bulk_request(struct usbd_class_data *const c_data,
			    struct net_buf *const buf, const int err)
{
	struct udc_buf_info *bi = (struct udc_buf_info *)net_buf_user_data(buf);

	if (bi->ep == USB_BULK_EP_IN) {
		net_buf_unref(buf);
		k_sem_give(&usb_bulk_tx_sem);
		return 0;
	}

	if (err) {
		net_buf_unref(buf);
		if (err != -ECONNABORTED) {
			usb_bulk_submit_out(c_data);
		}
		return 0;
	}

	k_fifo_put(&usb_bulk_rx_fifo, buf);

	return 0;
}

static void *usb_bulk_get_desc(struct usbd_class_data *const c_data,
			       const enum usbd_speed speed)
{
	struct usb_bulk_data *data = usbd_class_get_private(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return data->hs_desc;
	}

	return data->fs_desc;
}

static void usb_bulk_enable(struct usbd_class_data *const c_data)
{
	struct usb_bulk_data *data = usbd_class_get_private(c_data);

	if (atomic_test_and_set_bit(&data->state, USB_BULK_STATE_ENABLED)) {
		return;
	}

	for (int i = 0; i < CONFIG_SPI_FLASHER_USB_OUT_DEPTH; i++) {
		if (usb_bulk_submit_out(c_data)) {
			break;
		}
	}

	LOG_INF("Bulk interface enabled");
}

static void usb_bulk_disable(struct usbd_class_data *const c_data)
{
	struct usb_bulk_data *data = usbd_class_get_private(c_data);

	atomic_clear_bit(&data->state, USB_BULK_STATE_ENABLED);
	LOG_INF("Bulk interface disabled");
}

static int usb_bulk_class_init(struct usbd_class_data *const c_data)
{
	struct usb_bulk_data *data = usbd_class_get_private(c_data);

	data->c_data = c_data;

	return 0;
}

struct usbd_class_api usb_bulk_api = {
	.request = usb_bulk_request,
	.get_desc = usb_bulk_get_desc,
	.enable = usb_bulk_enable,
	.disable = usb_bulk_disable,
	.init = usb_bulk_class_init,
};

USBD_DEFINE_CLASS(spi_flasher_bulk, &usb_bulk_api, &usb_bulk_data, NULL);

struct net_buf *usb_bulk_rx_get(k_timeout_t timeout)
{
	return k_fifo_get(&usb_bulk_rx_fifo, timeout);
}

void usb_bulk_rx_put(struct net_buf *buf)
{
	net_buf_unref(buf);
	usb_bulk_submit_out(usb_bulk_data.c_data);
}

struct net_buf *usb_bulk_tx_alloc(size_t len, k_timeout_t timeout)
{
	struct usbd_class_data *c_data = usb_bulk_data.c_data;
	struct net_buf *buf;

	if (c_data == NULL || len > USB_BULK_IN_LEN) {
		return NULL;
	}

	if (k_sem_take(&usb_bulk_tx_sem, timeout)) {
		return NULL;
	}

	buf = usb_bulk_buf_alloc(USB_BULK_EP_IN);
	if (buf == NULL) {
		k_sem_give(&usb_bulk_tx_sem);
		return NULL;
	}

	return buf;
}

int usb_bulk_tx_submit(struct net_buf *buf)
{
	int err;

	err = usbd_ep_enqueue(usb_bulk_data.c_data, buf);
	if (err) {
		LOG_ERR("Failed to enqueue bulk IN (%d)", err);
		net_buf_unref(buf);
		k_sem_give(&usb_bulk_tx_sem);
	}

	return err;
}
