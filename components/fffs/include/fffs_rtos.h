#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fffs.h"
#include "esp_err.h"
#include "esp_log.h"

typedef struct fffs_head
{
    fffs_volume_t *vol;
    SemaphoreHandle_t xSemaphore;
} fffs_head_t;


fffs_head_t *fffs_rt_Init(fffs_volume_t *vol);

uint16_t fffs_rt_read_binary(fffs_head_t *fffs_head, uint32_t message_num, uint8_t *message);
esp_err_t fffs_rt_write_binary(fffs_head_t *fffs_head, uint8_t *message, int message_length);
esp_err_t fffs_rt_erase(fffs_head_t *fffs_head, int message_num);
esp_err_t fffs_rt_update(fffs_head_t *fffs_head, int message_num, uint8_t *new_message);