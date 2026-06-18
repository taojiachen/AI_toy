//
// Created by dirki on 04.07.25.
//
#include "pn532_driver.h"
#include "pn532_driver_spi.h"

#include <string.h>
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

static const char TAG[] = "pn532_driver_spi";

#define OP_READ_STATUS 0x02
#define OP_WRITE_DATA 0x01
#define OP_READ_DATA 0x03

typedef struct {
    gpio_num_t miso;
    gpio_num_t mosi;
    gpio_num_t sck;
    gpio_num_t cs;

    spi_host_device_t spi_host;
    spi_device_handle_t spi_handle;
    int32_t clock_frequency;
    bool bus_initialized;
    uint8_t frame_buffer[256];
} pn532_spi_driver_config;

static esp_err_t pn532_init_io(pn532_io_handle_t io_handle);
static void pn532_release_driver(pn532_io_handle_t io_handle);
static void pn532_release_io(pn532_io_handle_t io_handle);
static esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms);
static esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms);
static esp_err_t pn532_is_ready(pn532_io_handle_t io_handle);

esp_err_t pn532_new_driver_spi(gpio_num_t miso,
                               gpio_num_t mosi,
                               gpio_num_t sck,
                               gpio_num_t cs,
                               gpio_num_t reset,
                               gpio_num_t irq,
                               spi_host_device_t spi_host,
                               int32_t clock_frequency,
                               pn532_io_handle_t io_handle)
{
    if (io_handle == NULL)
        return ESP_ERR_INVALID_ARG;

    if (spi_host == SPI_HOST_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    pn532_spi_driver_config *dev_config = heap_caps_calloc(1, sizeof(pn532_spi_driver_config), MALLOC_CAP_DEFAULT);
    if (dev_config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    io_handle->reset = reset;
    io_handle->irq = irq;

    dev_config->spi_host = spi_host;
    dev_config->clock_frequency = clock_frequency;
    if (clock_frequency == 0) {
        dev_config->clock_frequency = 4000000;
    }
    dev_config->miso = miso;
    dev_config->mosi = mosi;
    dev_config->sck = sck;
    dev_config->cs = cs;
    dev_config->bus_initialized = false;
    dev_config->spi_handle = NULL;
    io_handle->driver_data = dev_config;

    io_handle->pn532_init_io = pn532_init_io;
    io_handle->pn532_release_io = pn532_release_io;
    io_handle->pn532_release_driver = pn532_release_driver;
    io_handle->pn532_read = pn532_read;
    io_handle->pn532_write = pn532_write;
    io_handle->pn532_init_extra = NULL;
    io_handle->pn532_is_ready = pn532_is_ready;

#ifdef CONFIG_ENABLE_IRQ_ISR
    io_handle->IRQQueue = NULL;
#endif

    ESP_LOGD(TAG, "SPI driver initialized");
    return ESP_OK;
}

void pn532_release_driver(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL)
        return;

    pn532_release_io(io_handle);

    free(io_handle->driver_data);
    io_handle->driver_data = NULL;
}

void spi_pre_cb(spi_transaction_t *trans) {
    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)trans->user;
    gpio_set_level(driver_config->cs, 0);
    esp_rom_delay_us(100);
}

void spi_post_cb(spi_transaction_t *trans) {
    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)trans->user;
    gpio_set_level(driver_config->cs, 1);
}

esp_err_t pn532_init_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "pn532_init_io() ...");
    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)io_handle->driver_data;

    if (driver_config->spi_handle != NULL || driver_config->bus_initialized) {
        pn532_release_io(io_handle);
    }

    if (driver_config->cs == GPIO_NUM_NC) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (driver_config->sck != GPIO_NUM_NC &&
        driver_config->miso != GPIO_NUM_NC &&
        driver_config->mosi != GPIO_NUM_NC)
    {
        spi_bus_config_t bus_config = {
            .miso_io_num = driver_config->miso,
            .mosi_io_num = driver_config->mosi,
            .sclk_io_num = driver_config->sck,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
        };
        err = spi_bus_initialize(driver_config->spi_host, &bus_config, SPI_DMA_CH_AUTO);
        ESP_LOGD(TAG, "pn532_init_io() spi_bus_initialize -> %d", err);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Failed to initialize SPI bus, maybe already initialzed");
            }
            else {
                ESP_LOGE(TAG, "Failed to initialize SPI bus");
                return err;
            }
        }
        else {
            driver_config->bus_initialized = true;
        }
    }

    if (driver_config->cs != GPIO_NUM_NC) {
        // configure NSS pin
        gpio_config_t io_conf;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = ((1ULL) << driver_config->cs);
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en = 0;
        if (gpio_config(&io_conf) != ESP_OK)
            return ESP_FAIL;
        gpio_set_level(driver_config->cs, 1);
    }

    spi_device_interface_config_t dev_config = {
        .address_bits = 0,
        .command_bits = 8,
        .dummy_bits = 0,
        .mode = 0,
        .clock_source = SPI_CLK_SRC_DEFAULT,
        .clock_speed_hz = (int)driver_config->clock_frequency,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_BIT_LSBFIRST,
        .queue_size = 1,
        .pre_cb = spi_pre_cb,
        .post_cb = spi_post_cb,
    };
    err = spi_bus_add_device(driver_config->spi_host, &dev_config, &driver_config->spi_handle);
    ESP_LOGD(TAG, "pn532_init_io() spi_bus_add_device -> %d", err);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device");
        return err;
    }

    return ESP_OK;
}

void pn532_release_io(pn532_io_handle_t io_handle)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return;
    }

    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)io_handle->driver_data;

    gpio_set_level(driver_config->cs, 1);

    if (driver_config->spi_handle != NULL) {
        ESP_LOGD(TAG, "remove SPI device ...");
        spi_bus_remove_device(driver_config->spi_handle);
        driver_config->spi_handle = NULL;
    }

    if (driver_config->bus_initialized) {
        driver_config->bus_initialized = false;
        spi_bus_free(driver_config->spi_host);
    }
}

esp_err_t pn532_is_ready(pn532_io_handle_t io_handle)
{
    uint8_t status;
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)io_handle->driver_data;
    esp_err_t result = spi_device_polling_transmit(driver_config->spi_handle,
        &(spi_transaction_t) {
            .cmd = OP_READ_STATUS,
            .rxlength = 8,
            .rx_buffer = &status,
            .user = io_handle->driver_data,
        });

    if (result != ESP_OK)
        return result;

    return ((status & 0x01) == 0x01) ? ESP_OK : ESP_FAIL;
}

esp_err_t pn532_read(pn532_io_handle_t io_handle, uint8_t *read_buffer, size_t read_size, int xfer_timeout_ms)
{
    static uint8_t rx_buffer[1];

    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = (xfer_timeout_ms > 0) ? pdMS_TO_TICKS(xfer_timeout_ms) : portMAX_DELAY;
    TickType_t elapsed_ticks = 0;

    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)io_handle->driver_data;

    esp_err_t result = ESP_FAIL;
    bool is_ready = false;
    while (!is_ready && elapsed_ticks < timeout_ticks) {
        result = spi_device_polling_transmit(driver_config->spi_handle,
            &(spi_transaction_t) {
                .cmd = OP_READ_STATUS,
                .rxlength = 8,
                .rx_buffer = rx_buffer,
                .user = io_handle->driver_data,
            });
        if (result == ESP_OK && (rx_buffer[0] & 0x01) == 0x01) {
            is_ready = true;
        }
        elapsed_ticks = xTaskGetTickCount() - start_ticks;
    }

    if (result != ESP_OK)
        return result;

    result = spi_device_polling_transmit(driver_config->spi_handle,
        &(spi_transaction_t) {
            .cmd = OP_READ_DATA,
            .rxlength = read_size * 8,
            .rx_buffer = read_buffer,
            .user = io_handle->driver_data,
        });

    return result;
}

esp_err_t pn532_write(pn532_io_handle_t io_handle, const uint8_t *write_buffer, size_t write_size, int xfer_timeout_ms)
{
    if (io_handle == NULL || io_handle->driver_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (write_size > 254) {
        return ESP_ERR_INVALID_SIZE;
    }

    pn532_spi_driver_config *driver_config = (pn532_spi_driver_config *)io_handle->driver_data;

    driver_config->frame_buffer[0] = 0;
    memcpy(driver_config->frame_buffer + 1, write_buffer, write_size);
    driver_config->frame_buffer[write_size + 1] = 0;

    return spi_device_polling_transmit(driver_config->spi_handle,
        &(spi_transaction_t) {
            .cmd = OP_WRITE_DATA,
            .length = (write_size + 2) * 8,
            .tx_buffer = driver_config->frame_buffer,
            .user = io_handle->driver_data,
        });
}
