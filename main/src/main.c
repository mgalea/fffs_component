#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fffs.h"
#include "fffs_disk.h"
#include "fffs_utils.h"
#include "fffs_rtos.h"

void read_blocks(void *fffs_head, int blocks)
{
    for (int i = 0; i < blocks; i++)
    {
        //srand(esp_timer_get_time());
        //int message_base = 10 + rand() % ((((fffs_head_t *)fffs_head)->vol->message_id) - 10);
        //uint8_t *message = calloc(fffs_head_read_binary((fffs_head_t *)fffs_head, i, NULL), 1);
        //printf("%d ", message_base);
        fffs_read_block(((fffs_head_t *)fffs_head)->vol, i);
        print_Message2ASC(((fffs_head_t *)fffs_head)->vol->read_buf, 512);

        //free(message);
    }

    //vTaskDelete(0);
}

void read_messages(void *fffs_head)
{
    int message_num;
    uint8_t *message;
    while (1)
    {
        srand(esp_timer_get_time());
        message_num = rand() % ((((fffs_head_t *)fffs_head)->vol->message_id) - 1);
        //printf("%d\n", message_num);
        message = calloc(fffs_rt_read_binary((fffs_head_t *)fffs_head, message_num, NULL), 1);
        print_Message2ASC(message, fffs_rt_read_binary((fffs_head_t *)fffs_head, message_num, message));

        free(message);
        fflush(stdout);
    }
    vTaskDelete(0);
}

static const char *TAG = "APP";

void write_messages(void *fffs_head)
{
    char template[] = "\
Had my friends Muse grown with this growing age \
He would be perjured, murderous, bloody and full of blame,\
The ersthwhile Drugs poisoned him that he fell sick of the world.\
To say, within thine own deep sunken eyes, I have no fault in this,\
Who, even but now come back again, assured! \
No Such thing as a free meal, to him that \
suffered the cursed tongue of the orator";

    srand(esp_timer_get_time());

    char message[400];
    int message_size;

    uint32_t i = 0;
    while (i++ < 100000)
    {
        message_size = (21 + rand() % 360);
        sprintf(message, "ID:%05u:Length: %02u %s", ((fffs_head_t *)fffs_head)->vol->message_id, message_size, template);
        fffs_rt_write_binary((fffs_head_t *)fffs_head, (uint8_t *)message, message_size);
        vTaskDelay(10);
    }
    printf("Ready writing.\n");
    vTaskDelete(0);
}

void app_main(void)
{
    sdmmc_card_t *s_card = sd_card_init();
    if (s_card == NULL)
        goto err;

    fffs_volume_t *fffs_vol = fffs_init(s_card, true);
    if (fffs_vol == NULL)
        goto err;

    fffs_head_t *sas_log = fffs_rt_Init(fffs_vol);

    ESP_LOGI(TAG, "Partitions size (%d) %d bytes.", fffs_vol->partition_size, fffs_vol->partition_size * (PARTITION_SIZE)*SD_BLOCK_SIZE);
    ESP_LOGI(TAG, "Sector size (%d) %d bytes.", fffs_vol->sector_size, fffs_vol->sector_size * (SECTOR_SIZE)*SD_BLOCK_SIZE);
    ESP_LOGI(TAG, "Current Partition ID: %d.", fffs_vol->current_partition);
    ESP_LOGI(TAG, "Current Block ID: %d.", (uint32_t)fffs_vol->current_block);
    ESP_LOGI(TAG, "Current Message ID: %d.\n", (uint32_t)fffs_vol->message_id);

    ESP_LOGI(TAG, "Current Sector ID: %d.", (uint32_t)fffs_vol->current_sector);
    ESP_LOGI(TAG, "Current Message Index ID: %d.", (uint32_t)fffs_vol->block_index);
    ESP_LOGI(TAG, "Number of Messages in last Block: %d.", (uint32_t)fffs_vol->messages_in_block);

    //fffs_format(fffs_vol,1,1,false);
    printf("Writing blocks.\n");
    //write_messages(sas_log);
    //fffs_rt_erase(sas_log, 10);
/*
    uint8_t *message = malloc(512);
    uint8_t size = fffs_rt_read_binary(sas_log, 11, message);
    uint8_t  s[510];

    for (int i = 0; i < size; i++)
    {
        s[i] = *(message + size - i - 1);
    }
   
    fffs_rt_update(sas_log, 11, s);

    print_Message2ASC(message, fffs_rt_read_binary(sas_log, 11, message));
*/
    xTaskCreate(read_messages, "sas_log", 4096, sas_log, 5, NULL);
    xTaskCreate(write_messages, "mqtt_log", 4096, sas_log, 6, NULL);

    while (1)
    {
        vTaskDelay(50);
        //ESP_LOGI(TAG,"Time: %d", esp_timer_get_time());
    }

err:
    sd_card_deinit(s_card);
}