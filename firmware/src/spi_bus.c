/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "spi_bus.h"

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(spi_bus, CONFIG_LOG_DEFAULT_LEVEL);

#define SPI_BUS_OPERATION (SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER)

#define SPI_BUS_TARGET_EXTERNAL DT_NODELABEL(target_external)

#define SPI_BUS_PRESCALER_MIN_SHIFT 1
#define SPI_BUS_PRESCALER_MAX_SHIFT 8

struct spi_bus {
	const struct device *dev;
	struct spi_config config[2];
	uint8_t config_index;
	uint32_t frequency;
	spi_operation_t operation;
	uint32_t (*freq_round)(uint32_t requested);
};

static uint32_t spi_bus_external_freq_round(uint32_t requested);

#define SPI_BUS_DEFINE(node, round_fn)				\
	{							\
		.dev = DEVICE_DT_GET(DT_BUS(node)),		\
		.config = {					\
			SPI_CONFIG_DT(node, SPI_BUS_OPERATION),	\
			SPI_CONFIG_DT(node, SPI_BUS_OPERATION),	\
		},						\
		.freq_round = round_fn,				\
	}

static struct spi_bus spi_buses[SPI_BUS_COUNT] = {
	[SPI_BUS_EXTERNAL] = SPI_BUS_DEFINE(SPI_BUS_TARGET_EXTERNAL,
					    spi_bus_external_freq_round),
};

static struct spi_bus *spi_bus_active = &spi_buses[SPI_BUS_EXTERNAL];

static uint32_t spi_bus_external_freq_round(uint32_t requested)
{
	static const struct stm32_pclken pclken[] =
		STM32_DT_CLOCKS(DT_BUS(SPI_BUS_TARGET_EXTERNAL));
	const size_t index = ARRAY_SIZE(pclken) > 1 ? 1 : 0;
	uint32_t clock;
	int shift;

	if (clock_control_get_rate(DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE),
				   (clock_control_subsys_t)&pclken[index], &clock) < 0) {
		return 0;
	}

	for (shift = SPI_BUS_PRESCALER_MIN_SHIFT; shift <= SPI_BUS_PRESCALER_MAX_SHIFT; shift++) {
		if ((clock >> shift) <= requested) {
			return clock >> shift;
		}
	}

	return 0;
}

static struct spi_config *spi_bus_config(struct spi_bus *bus)
{
	struct spi_config *config = &bus->config[bus->config_index];

	if (config->frequency == bus->frequency && config->operation == bus->operation) {
		return config;
	}

	bus->config_index ^= 1;
	config = &bus->config[bus->config_index];
	config->frequency = bus->frequency;
	config->operation = bus->operation;

	return config;
}

int spi_bus_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(spi_buses); i++) {
		struct spi_bus *bus = &spi_buses[i];

		if (!device_is_ready(bus->dev)) {
			LOG_ERR("SPI bus %u (%s) not ready", (unsigned int)i, bus->dev->name);
			return -ENODEV;
		}

		bus->frequency = bus->freq_round(bus->config[0].frequency);
		if (bus->frequency == 0) {
			LOG_ERR("SPI bus %u cannot reach %u Hz", (unsigned int)i,
				bus->config[0].frequency);
			return -EINVAL;
		}

		bus->operation = bus->config[0].operation;
	}

	return 0;
}

int spi_bus_select(uint8_t index)
{
	if (index >= ARRAY_SIZE(spi_buses)) {
		return -EINVAL;
	}

	spi_bus_active = &spi_buses[index];

	return 0;
}

uint32_t spi_bus_freq_set(uint32_t requested)
{
	struct spi_bus *bus = spi_bus_active;
	uint32_t actual;

	if (requested == 0) {
		return 0;
	}

	actual = bus->freq_round(requested);
	if (actual == 0) {
		return 0;
	}

	bus->frequency = actual;

	return actual;
}

uint32_t spi_bus_freq_get(void)
{
	return spi_bus_active->frequency;
}

int spi_bus_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
	struct spi_bus *bus = spi_bus_active;
	struct spi_buf tx_buf[2];
	struct spi_buf rx_buf[2];
	struct spi_buf_set tx_set = { .buffers = tx_buf };
	struct spi_buf_set rx_set = { .buffers = rx_buf };

	if (tx_len == 0 && rx_len == 0) {
		return 0;
	}

	if (tx_len > 0) {
		tx_buf[tx_set.count] = (struct spi_buf){ .buf = (void *)tx, .len = tx_len };
		rx_buf[rx_set.count] = (struct spi_buf){ .buf = NULL, .len = tx_len };
		tx_set.count++;
		rx_set.count++;
	}

	if (rx_len > 0) {
		tx_buf[tx_set.count] = (struct spi_buf){ .buf = NULL, .len = rx_len };
		rx_buf[rx_set.count] = (struct spi_buf){ .buf = rx, .len = rx_len };
		tx_set.count++;
		rx_set.count++;
	}

	return spi_transceive(bus->dev, spi_bus_config(bus), &tx_set,
			      rx_len > 0 ? &rx_set : NULL);
}
