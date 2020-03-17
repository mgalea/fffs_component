#include "esp_err.h"
#include "esp_log.h"

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "fffs_disk.h"

#define USE_SPI_MODE
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 4

#define DISK_CHECK(a, str, goto_tag, ...)                                         \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

static const char *TAG = "FFFS_DISK";

sdmmc_card_t *sd_card_init()
{
    sdmmc_card_t *s_card = NULL;

    ESP_LOGI(TAG, "Initializing SD card");
    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    sdspi_slot_config_t slot_config = SDSPI_SLOT_CONFIG_DEFAULT();

    slot_config.gpio_miso = PIN_NUM_MISO;
    slot_config.gpio_mosi = PIN_NUM_MOSI;
    slot_config.gpio_sck = PIN_NUM_CLK;
    slot_config.gpio_cs = PIN_NUM_CS;

    esp_err_t err = ESP_OK;

    s_card = malloc(sizeof(sdmmc_card_t));
    if (s_card == NULL)
    {
        ESP_LOGD(TAG, "SD memory allocation failed.");
        goto fail;
    }

    err = (*host.init)();

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "host init returned rc=0x%x", err);
        goto fail;
    }

    err = sdspi_host_init_slot(host.slot, &slot_config);

    if (err != ESP_OK)
    {
        ESP_LOGD(TAG, "slot_config returned rc=0x%x", err);
        goto fail;
    }

    err = sdmmc_card_init(&host, s_card);

    DISK_CHECK(err==0, "Insert SD card and restart.", fail);

        // Card has been initialized, print its properties

    sdmmc_card_print_info(stdout, s_card);

    return s_card;

fail:
    free(s_card);

    return NULL;
}

esp_err_t sd_card_deinit(sdmmc_card_t *s_card)
{
    if (s_card == NULL)
        return ESP_OK;
    free(s_card);
    return ESP_OK;
}