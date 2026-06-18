//
// Created by dirki on 04.07.25.
//

#ifndef PN532_DRIVER_SPI_H
#define PN532_DRIVER_SPI_H

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "pn532_driver.h"

#ifdef __cplusplus
extern "C"
{
#endif

esp_err_t pn532_new_driver_spi(gpio_num_t miso,
                               gpio_num_t mosi,
                               gpio_num_t sck,
                               gpio_num_t cs,
                               gpio_num_t reset,
                               gpio_num_t irq,
                               spi_host_device_t spi_host,
                               int32_t clock_frequency,
                               pn532_io_handle_t io_handle);

#ifdef __cplusplus
}
#endif

#endif //PN532_DRIVER_SPI_H
