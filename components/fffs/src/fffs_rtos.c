#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "fffs.h"
#include "fffs_rtos.h"
#include "fffs_utils.h"
static char *TAG = "FSRTOS";

#define FRTOS_CHECK(a, str, goto_tag, ...)                                        \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

fffs_head_t *fffs_rt_Init(fffs_volume_t *vol)
{

    fffs_head_t *fffs_head = malloc(sizeof(fffs_head_t));
    FRTOS_CHECK(fffs_head, "Cannot assign memory for fs head.", err);

    fffs_head->vol = vol;
    fffs_head->xSemaphore = NULL;
    fffs_head->xSemaphore = xSemaphoreCreateMutex();
    FRTOS_CHECK(fffs_head->xSemaphore, "Cannot assign semaphore for fs head.", err);

    if (fffs_head->xSemaphore == NULL)
        FRTOS_CHECK(fffs_head->xSemaphore, "Cannot get MUTEX", err);

    return fffs_head;
err:
    return NULL;
}

uint16_t fffs_rt_read_binary(fffs_head_t *fffs_head, uint32_t message_num, uint8_t *message) //use an unsigned int so to use directly into a malloc
{
    int message_length = 0;
    FRTOS_CHECK(fffs_head, "Head cannot be NULL.", err);
    FRTOS_CHECK(fffs_head->xSemaphore, "Semaphore cannot be NULL.", err);

obtain_semaphore:
    vTaskDelay(10);

    FRTOS_CHECK((xSemaphoreTake(fffs_head->xSemaphore, pdMS_TO_TICKS(200)) == pdTRUE), "Cannot obtain semaphore.", obtain_semaphore);

    FRTOS_CHECK(fffs_read(fffs_head->vol, message_num, message, &message_length) == ESP_OK, "Cannot read message", err);

release_semaphore:
    FRTOS_CHECK(xSemaphoreGive(fffs_head->xSemaphore) == pdTRUE, "Cannot release semaphore", release_semaphore);
err:
    return message_length;
}

esp_err_t fffs_rt_write_binary(fffs_head_t *fffs_head, uint8_t *message, int message_length)
{
    FRTOS_CHECK(fffs_head, "Head cannot be NULL.", err);
    FRTOS_CHECK(fffs_head->xSemaphore, "Semaphore cannot be NULL.", err);
    FRTOS_CHECK(message_length > 0 && message_length < 510, "Invalid message size", err);
    FRTOS_CHECK(message != NULL, "Message is NULL", err);
obtain_semaphore:
    FRTOS_CHECK((xSemaphoreTake(fffs_head->xSemaphore, pdMS_TO_TICKS(200)) == pdTRUE), "Cannot obtain semaphore.", obtain_semaphore);

    FRTOS_CHECK(fffs_write(fffs_head->vol, message, message_length) == ESP_OK, "Cannot write message", err);
release_semaphore:
    FRTOS_CHECK(xSemaphoreGive(fffs_head->xSemaphore) == pdTRUE, "Cannot release semaphore", release_semaphore);

    return ESP_OK;

err:
    return ESP_FAIL;
}

esp_err_t fffs_rt_erase(fffs_head_t *fffs_head, int message_num)
{
    FRTOS_CHECK(fffs_head, "Head cannot be NULL.", err);
    FRTOS_CHECK(fffs_head->xSemaphore, "Semaphore cannot be NULL.", err);
    FRTOS_CHECK(message_num > 0 && message_num < fffs_head->vol->message_id, "Invalid message number", err);

obtain_semaphore:
    FRTOS_CHECK((xSemaphoreTake(fffs_head->xSemaphore, pdMS_TO_TICKS(200)) == pdTRUE), "Cannot obtain semaphore.", obtain_semaphore);

    FRTOS_CHECK(fffs_erase(fffs_head->vol, message_num) == ESP_OK, "Cannot write message", err);

release_semaphore:
    FRTOS_CHECK(xSemaphoreGive(fffs_head->xSemaphore) == pdTRUE, "Cannot release semaphore", release_semaphore);

    return ESP_OK;

err:
    return ESP_FAIL;
}

esp_err_t fffs_rt_update(fffs_head_t *fffs_head, int message_num, uint8_t *new_message)
{
    FRTOS_CHECK(fffs_head, "Head cannot be NULL.", err);
    FRTOS_CHECK(fffs_head->xSemaphore, "Semaphore cannot be NULL.", err);
    FRTOS_CHECK(message_num > 0 && message_num < fffs_head->vol->message_id, "Invalid message number", err);

obtain_semaphore:
    FRTOS_CHECK((xSemaphoreTake(fffs_head->xSemaphore, pdMS_TO_TICKS(200)) == pdTRUE), "Cannot obtain semaphore.", obtain_semaphore);
    FRTOS_CHECK(fffs_update(fffs_head->vol, message_num, new_message) == ESP_OK, "Cannot write message", err);

release_semaphore:
    FRTOS_CHECK(xSemaphoreGive(fffs_head->xSemaphore) == pdTRUE, "Cannot release semaphore", release_semaphore);

    return ESP_OK;

err:
    return ESP_FAIL;
}