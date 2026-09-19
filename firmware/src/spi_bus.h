/*
 * Copyright (c) 2026 John Crispin <john@phrozen.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPI_FLASHER_SPI_BUS_H
#define SPI_FLASHER_SPI_BUS_H

#include <stddef.h>
#include <stdint.h>

#define SPI_BUS_EXTERNAL 0
#define SPI_BUS_COUNT    1

int spi_bus_init(void);
int spi_bus_select(uint8_t index);
uint32_t spi_bus_freq_set(uint32_t requested);
uint32_t spi_bus_freq_get(void);
int spi_bus_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len);

#endif
