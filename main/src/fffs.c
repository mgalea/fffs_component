/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"

#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "fffs.h"
#include "fffs_utils.h"
#include "fffs_disk.h"

#define FFFS_CHECK(a, str, goto_tag, ...)                                         \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

static const char *TAG = "FFFS";

static esp_err_t fffs_erase_block(fffs_volume_t *fffs_volume, size_t block, size_t num)
{
    FFFS_CHECK(fffs_volume, "Volume is Null.", err);

    for (int i = 0; i < SD_BLOCK_SIZE; i++)
        *((char *)fffs_volume->read_buf + i) = 0;

    for (; num > 0; num--)
    {
        if (num % 100 == 0)
            printf(".");

        fflush(stdout);
        sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, block++, 1);
    }

    return ESP_OK;

err:
    return ESP_FAIL;
}

static esp_err_t fffs_update_partition_block(fffs_volume_t *fffs_volume)
{
    ESP_LOGI(TAG, "Current partition %d", fffs_volume->current_partition);
    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition * (fffs_volume->partition_size * PARTITION_SIZE), 1), "Cannot read partition ", fail);
    (((fffs_partition_table_t *)fffs_volume->read_buf)->jump_to_next_partition) = true; //This is always TRUE except when formatting the SD card
    FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition * (fffs_volume->partition_size * PARTITION_SIZE), 1), "Cannot write partition", fail);
    fffs_volume->current_partition++;
    return ESP_OK;

fail:
    return ESP_FAIL;
}

static esp_err_t fffs_create_sector_block(fffs_volume_t *fffs_volume)
{

    /* Update the old sector before creating a new one */

    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot read sector ", fail);

    (((fffs_partition_table_t *)fffs_volume->read_buf)->jump_to_next_sector) = true; //This is always TRUE except when formatting the SD card

    FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot write sector ", fail);

    /* Now we move to the new sector */

    (((fffs_partition_table_t *)fffs_volume->read_buf)->jump_to_next_sector) = false;
    (((fffs_partition_table_t *)fffs_volume->read_buf)->partition_id) = fffs_volume->current_partition;
    (((fffs_partition_table_t *)fffs_volume->read_buf)->magic_number) = FFFS_MAGIC_NUMBER;
    (((fffs_sector_table_t *)fffs_volume->read_buf)->first_message) = fffs_volume->message_id;

    fffs_volume->current_sector = fffs_volume->current_block;
    fffs_volume->messages_in_block = 0;
    fffs_volume->block_index = 0;
    for (int i = 0; i < ((SECTOR_SIZE) / (BLOCKS_IN_SECTOR)); i++)
    {
        ((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[i] = 0;
    }

    FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot write sector", fail);

    /* Update the old sector before creating a new one */

    return ESP_OK;

fail:
    return ESP_FAIL;
}

esp_err_t fffs_format(fffs_volume_t *fffs_volume, unsigned char partition_size, unsigned char sector_size, bool message_rotate)
{
    esp_err_t err = ESP_FAIL;
    fffs_sector_table_t *sector_table = calloc(1, sizeof(fffs_sector_table_t)); //Declared in this way to ensure the entire sector table is initalized

    ((fffs_partition_table_t *)sector_table)->jump_to_next_partition = false;
    ((fffs_partition_table_t *)sector_table)->jump_to_next_sector = false;
    ((fffs_partition_table_t *)sector_table)->card_full = false;
    ((fffs_partition_table_t *)sector_table)->message_rotate = false;
    ((fffs_partition_table_t *)sector_table)->last_block = 1;
    ((fffs_partition_table_t *)sector_table)->sector_size = sector_size == 0 ? 1 : sector_size;
    ((fffs_partition_table_t *)sector_table)->magic_number = FFFS_MAGIC_NUMBER;
    ((fffs_partition_table_t *)sector_table)->partition_size = partition_size == 0 ? 1 : partition_size;
    ((fffs_partition_table_t *)sector_table)->partition_id = 0;

    for (uint64_t i = 0; i < fffs_volume->sd_card->csd.capacity; i = i + (partition_size * (PARTITION_SIZE)))
    {
        fffs_erase_block(fffs_volume, i, sector_size * (SECTOR_SIZE));
        ESP_LOGI(TAG, "Creating Partition: %d at block number %d", ((fffs_partition_table_t *)sector_table)->partition_id, (uint32_t)i);

        memcpy(fffs_volume->read_buf, sector_table, sizeof(fffs_sector_table_t));
        FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, i, 1) == ESP_OK, "Cannot format sector", fail);

        ((fffs_partition_table_t *)sector_table)->partition_id++;
    }

    ESP_LOGI(TAG, "Created %d Partitions of size %d bytes.", ((fffs_partition_table_t *)sector_table)->partition_id, partition_size * (PARTITION_SIZE)*512);
    fffs_volume->last_block = 1;
    fffs_volume->current_block = 1;
    err = ESP_OK;

fail:
    free(sector_table);
    return err;
}

static uint32_t fffs_find_lastBlock(fffs_volume_t *fffs_vol)
{
    fffs_vol->last_block = 0;

    FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, 0, 1) == ESP_OK, "Cannot read partition.", fail);

    if ((((fffs_partition_table_t *)fffs_vol->read_buf)->magic_number) == FFFS_MAGIC_NUMBER)
    {
        ESP_LOGI(TAG, "FFS: Found Boot Partition.");

        fffs_vol->partition_size = ((fffs_partition_table_t *)fffs_vol->read_buf)->partition_size == 0 ? 1 : ((fffs_partition_table_t *)fffs_vol->read_buf)->partition_size;
        fffs_vol->sector_size = ((fffs_partition_table_t *)fffs_vol->read_buf)->sector_size == 0 ? 1 : ((fffs_partition_table_t *)fffs_vol->read_buf)->sector_size;

        if ((((fffs_partition_table_t *)fffs_vol->read_buf)->card_full) == true)
        {
            ESP_LOGE(TAG, "SD Card is full!");
            goto fail;
        }

        fffs_vol->messages_in_block = 0;

        while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_partition) == true)
        {
            fffs_vol->current_partition++;
            fffs_vol->last_block = fffs_vol->current_partition * (fffs_vol->partition_size * PARTITION_SIZE);
            if (fffs_vol->last_block >= fffs_vol->sd_card->csd.capacity)
            {
                ESP_LOGE(TAG, "SD Card is full!");
                goto fail;
            }

            FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fffs_vol->last_block, 1) == ESP_OK, "Cannot read partition.", fail);
        }

        fffs_vol->current_sector = 0;

        while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_sector) == true)
        {
            fffs_vol->last_block = fffs_vol->last_block + (fffs_vol->sector_size * (SECTOR_SIZE));
            FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fffs_vol->last_block, 1) == ESP_OK, "Cannot read partition.", fail);
        }

        fffs_vol->current_sector = fffs_vol->last_block;
        fffs_vol->last_block = ((fffs_partition_table_t *)fffs_vol->read_buf)->last_block;
        fffs_vol->message_id = ((fffs_partition_table_t *)fffs_vol->read_buf)->message_id;

        fffs_vol->block_index = 0;
        while (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[fffs_vol->block_index + 1] > 0)
        {
            fffs_vol->block_index++;
        }
        fffs_vol->messages_in_block = ((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[fffs_vol->block_index];
    }

fail:
    return fffs_vol->last_block;
}

esp_err_t fffs_read_block(fffs_volume_t *fffs_volume, int block_num)
{
    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, block_num, 1) == ESP_OK, "Cannot read sector ", fail);
    return ESP_OK;

fail:
    return ESP_FAIL;
}

fffs_volume_t *fffs_init(sdmmc_card_t *s_card, bool format)
{
    size_t block_size = s_card->csd.sector_size; //not used at the moment. @todo need to change to sector_size

    fffs_volume_t *fffs_vol = malloc(sizeof(fffs_volume_t));
    FFFS_CHECK(fffs_vol, "Cannot create FFFS volume", err);

    fffs_vol->sd_card = s_card;
    fffs_vol->current_block = 0;
    fffs_vol->current_partition = 0;
    fffs_vol->current_sector = 0;
    fffs_vol->message_id = 0;
    fffs_vol->block_index = 0;
    fffs_vol->partition_size = 1;
    fffs_vol->sector_size = 1;
    fffs_vol->message_rotate = false;

    fffs_vol->read_buf = heap_caps_malloc(block_size, MALLOC_CAP_DMA);
    FFFS_CHECK(fffs_vol->read_buf, "Cannot create read/write buffer for FFFS volume", fail);

    ESP_LOGI(TAG, "Starting FF Filing System.");

    fffs_vol->current_block = fffs_find_lastBlock(fffs_vol);

    FFFS_CHECK(fffs_vol->current_block > 0, "SD Card is not formatted for FFFS.", format);
    return fffs_vol;

format:
    if (format)
        FFFS_CHECK(fffs_format(fffs_vol, 2, 1, false) == ESP_OK, "Formatting was not successful.", fail_format);

    return fffs_vol;

fail_format:
    ESP_LOGI(TAG, "Format failed,Changed SD card.");
    free(fffs_vol->read_buf);

fail:
    free(fffs_vol);

err:
    return NULL;
}

esp_err_t fffs_deinit(fffs_volume_t *fffs_vol)
{
    if (fffs_vol == NULL)
        return ESP_OK;
    heap_caps_free(fffs_vol->read_buf);
    free(fffs_vol);
    return ESP_OK;
}

static esp_err_t fffs_update_table(fffs_volume_t *fffs_volume)
{
    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot read sector ", fail);

    fffs_volume->last_block = fffs_volume->current_block;

    (((fffs_partition_table_t *)fffs_volume->read_buf)->last_block) = fffs_volume->last_block;
    (((fffs_partition_table_t *)fffs_volume->read_buf)->message_id) = fffs_volume->message_id;

    if (fffs_volume->messages_in_block == 0)
        ESP_LOGI(TAG, "MESSAGE IS 0 AT BLOCK %d in INDEX %d", fffs_volume->current_block, fffs_volume->block_index);

    (((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[fffs_volume->block_index]) = fffs_volume->messages_in_block;

    FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot write sector ", fail);

    return ESP_OK;
fail:
    return ESP_FAIL;
}

static esp_err_t fffs_next_block(fffs_volume_t *fffs_volume)
{
    FFFS_CHECK(fffs_volume->current_block++ < fffs_volume->sd_card->csd.capacity, "SD CARD is full.", full_card);

    FFFS_CHECK(fffs_erase_block(fffs_volume, fffs_volume->current_block, 1) == ESP_OK, "Cannot create next block", fail);

    if (fffs_volume->current_block % (fffs_volume->partition_size * (PARTITION_SIZE)) == 0)
    {
        fffs_update_partition_block(fffs_volume);
    }

    if (fffs_volume->current_block % (SECTOR_SIZE) == 0)
    {
        ESP_LOGI(TAG, "Creating new sector");
        fffs_create_sector_block(fffs_volume);
        fffs_volume->messages_in_block = 0;
        fffs_volume->block_index = 0;
        return fffs_next_block(fffs_volume);
    }

    if (fffs_volume->current_block % (BLOCKS_IN_SECTOR) == 0)
    {
        fffs_volume->last_block = fffs_volume->current_block;

        if (fffs_volume->messages_in_block > 0)
            fffs_volume->block_index++;
        fffs_volume->messages_in_block = 0;
        return ESP_OK;
    }

full_card:
    fffs_volume->current_partition = 0;
    fffs_volume->current_sector = 0;
    fffs_volume->current_block = 0;
    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition, 1) == ESP_OK, "Cannot read partition ", fail);
    ((fffs_partition_table_t *)fffs_volume->read_buf)->card_full = true;
    ((fffs_partition_table_t *)fffs_volume->read_buf)->jump_to_next_sector = false;
    FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition, 1) == ESP_OK, "Cannot read partition ", fail);

    if (((fffs_partition_table_t *)fffs_volume->read_buf)->message_rotate == true) //log can be rotated
    {
        ((fffs_partition_table_t *)fffs_volume->read_buf)->jump_to_next_partition = false;
        FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition, 1) == ESP_OK, "Cannot read partition ", fail);
        fffs_next_block(fffs_volume);
    }

fail:
    return ESP_FAIL;
}

esp_err_t fffs_write(fffs_volume_t *fffs_volume, void *message, int size)
{
    esp_err_t err = ESP_OK;
    if (size > SD_BLOCK_SIZE - 2 || size == 0) //messages can only 510 bytes long  since the first two bytes must be reserved for the next message offset
        return ESP_ERR_INVALID_SIZE;

    fffs_volume->current_block = fffs_volume->last_block;

    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_block, 1);

    int tmp = 0, i = 0;

    //Search for the last message written to the block
    do
    {
        tmp = *((uint8_t *)(fffs_volume->read_buf) + i);
        tmp = (tmp == 0 && *((uint8_t *)(fffs_volume->read_buf) + i + 1) > 0) ? (*((uint8_t *)(fffs_volume->read_buf) + i + 1)) + 0x100 : tmp;

        if ((i + tmp) >= (SD_BLOCK_SIZE - 2)) //2 is the size of the smallest message
        {
            tmp = 0;
        }
        else
        {
            i = i + tmp;
        }
    } while (tmp > 0);

    if ((SD_BLOCK_SIZE - 3 - (i)) < size)
    {
        if (fffs_next_block(fffs_volume) == ESP_FAIL)
            return ESP_FAIL;

        return fffs_write(fffs_volume, message, size) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    if (size < 255)
    {
        memcpy((uint8_t *)(fffs_volume->read_buf) + i + 1, message, size);
        *((uint8_t *)(fffs_volume->read_buf) + i) = (uint8_t)size + 1; //this is the offset not message size
    }
    else
    {
        memcpy((uint8_t *)(fffs_volume->read_buf) + i + 2, message, size);
        *((uint8_t *)(fffs_volume->read_buf) + i) = 0;                                //indicate that the message is longer than 255 characters
        *((uint8_t *)(fffs_volume->read_buf) + i + 1) = (uint8_t)((size - 0xff)) + 1; //this is the offset not message size
    }

    err = sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_block, 1);
    if (err != ESP_OK)
        return ESP_FAIL;

    fffs_volume->messages_in_block++;
    fffs_volume->message_id++;
    fffs_update_table(fffs_volume);

    return ESP_OK;
}

esp_err_t fffs_read(fffs_volume_t *fffs_vol, size_t message_num, uint8_t *message, int *size)
{

    uint32_t fetch_block;
    uint8_t partition = 0;
    int tmp_size;
    FFFS_CHECK((message_num < fffs_vol->message_id), "Message num is too big", err);

    do
    {
        fetch_block = (fffs_vol->partition_size * (PARTITION_SIZE)) * partition++;
        FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1) == ESP_OK, "Cannot read partition", err);

    } while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_partition) == true && (((fffs_partition_table_t *)fffs_vol->read_buf)->message_id < message_num));

    --partition;
    int message_base;

    while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_sector) == true && (((fffs_partition_table_t *)fffs_vol->read_buf)->message_id < message_num))
    {
        fetch_block = fetch_block + (SECTOR_SIZE);
        FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1) == ESP_OK, "Cannot read sector", err);
    }
    message_base = ((fffs_sector_table_t *)fffs_vol->read_buf)->first_message;

    fetch_block++;

    int old_message_base;

    int i = 0;
    do
    {
        old_message_base = message_base;
        message_base = message_base + (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[i++]);

    } while (message_base < ((message_num) + 1) && (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[i]) != 0);

    fetch_block = fetch_block + (i * BLOCKS_IN_SECTOR) - 1;

    FFFS_CHECK(sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1) == ESP_OK, "Cannot read block", err);

    int index = 0;
    for (unsigned char offset = (message_num - old_message_base); offset > 0; offset--)
    {
        index = index + (*((uint8_t *)(fffs_vol->read_buf) + index) == 0 ? 0xFF + (*((uint8_t *)(fffs_vol->read_buf) + index + 1)) : *((uint8_t *)(fffs_vol->read_buf) + index));
    }

    tmp_size = *((uint8_t *)(fffs_vol->read_buf) + index++) - 1;

    if (tmp_size == 0)
        tmp_size = *((uint8_t *)(fffs_vol->read_buf) + index) + 0xFF;

    *size = tmp_size;

    FFFS_CHECK(message != NULL, "Read length only", size_only);
    memcpy(message, (uint8_t *)(fffs_vol->read_buf) + index + ((*size) > 0xFF ? 1 : 0), *size);

size_only:
    return ESP_OK;

err:
    return ESP_FAIL;
}
