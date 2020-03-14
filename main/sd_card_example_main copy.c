/* SD card and FAT filesystem example.
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include <stdlib.h>
#include <time.h>
#include <ctype.h>

#define USE_SPI_MODE
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 4
#define SD_BLOCK_SIZE 512 //<DEFAULT SD CARD BLOCK size

#define KILOBYTE 1024
#define MEGABYTE KILOBYTE * 1024
#define GIGABYTE MEGABYTE * 1024

//Default sector boundary multiples
#define PARTITION_SIZE ((256 * MEGABYTE) / SD_BLOCK_SIZE) //Expressed in block size
#define SECTOR_SIZE ((128 * KILOBYTE) / SD_BLOCK_SIZE)    //Expressed in block size
#define BLOCKS_IN_SECTOR 1                                //This is the number of blocks reported per sector 1=all blacks ina sector

#define FFFS_CHECK(a, str, goto_tag, ...)                                         \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

#define FFFS_MAGIC_NUMBER 0xFFFFFFFEFDFDFBFB //< MAGIC NUMBER READS LFS001


static sdmmc_card_t *s_card = NULL;

typedef struct //__attribute__((packed))
{
    unsigned int jump_to_next_partition : 2; //<If value is 0 then this is the current partition - Each partition is 1 Gigabyte (1024*1024*2) sectors in size or PARTITION_SIZE. TRUE=jump
    unsigned int jump_to_next_sector : 2;    //< If value is 0x0 then this is the current sector otherwise goto next sector - A sector is 1024M/10; TRUE=jump
    unsigned int card_full : 2;              //<FLAG to  indicate the card is full. TRUE=full.
    unsigned int message_rotate : 2;         //<FLAG to indicate whether the messages are looped to the start if the SD card is full. TRUE = rotate
    uint8_t partition_size;                  //<size of the partitions measured in multiples of (256*1024*1024) blocks. 0 is default partition size (256 Megabytes)
    uint8_t sector_size;                     //<size of a sector in a partition in multiples of 256 blocks. 0 is the default sector size (256)
    uint8_t partition_id;                    //Partiton ID. There can be up to 256 partitions
    uint32_t last_block;                     //< If above value is 0x0 then this value points to the last block written in th partition
    uint32_t message_id;                     //<Last message written in the partition.
    uint64_t magic_number;

} fffs_partition_table_t;

typedef struct //struct __attribute__((packed))
{
    fffs_partition_table_t partition_sector_table; //<The sector table is made up of the boot_partition table first ....
    uint32_t first_message;
    uint8_t sector_message_index[(SECTOR_SIZE) / BLOCKS_IN_SECTOR]; //<folowed by the meesage offsets in each block in the sector
} fffs_sector_table_t;

typedef struct fffs_volume
{
    sdmmc_card_t *sd_card;
    uint8_t partition_size;
    uint8_t sector_size;
    void *read_buf;
    char current_partition;
    uint32_t current_sector;
    uint32_t current_block;
    uint32_t block_index;
    char messages_in_block;
    uint32_t message_id;
    bool message_rotate;
} fffs_volume_t;

static esp_err_t fffs_create_sector_block(fffs_volume_t *fffs_volume);

void print_Message2HEX(const unsigned char *message, size_t msgLength)
{
    time_t wtime = time(0);
    printf("[%d] [ ", (uint32_t)wtime);
    if (msgLength > 1024)
        msgLength = 1024;
    while (msgLength--)
        printf("%.2X ", (uint8_t)*message++);

    printf("] \n");
}

void print_Message2ASC(const unsigned char *message, size_t msgLength)
{
    time_t wtime = time(0);
    printf("[%d] [ ", (uint32_t)wtime);
    if (msgLength > 1024)
        msgLength = 1024;
    while (msgLength--)
        printf("%c", isprint((uint8_t)*message++) ? (uint8_t) * (message - 1) : 0x2E);

    printf("] \n");
}

static const char *TAG = "FFFS";

esp_err_t sd_card_init()
{
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
        err = ESP_ERR_NO_MEM;
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

    if ((uint64_t)s_card->csd.capacity == 0)
    {
        ESP_LOGD(TAG, "Insert SD card and restart.");
        goto fail;
    }
    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, s_card);
    printf("Total number of sectors: %llu\n", (uint64_t)s_card->csd.capacity);
    printf("Sectors size in bytes: %u\n", (uint32_t)s_card->csd.sector_size);
    printf("Max transfer speed: %u\n", (uint32_t)s_card->csd.tr_speed);
    printf("Maximum Freq KHz: %d!\n", s_card->max_freq_khz);

    return ESP_OK;

fail:
    free(s_card);
    err = ESP_FAIL;

    return err;
}

esp_err_t sd_card_deinit()
{
    if (s_card == NULL)
        return ESP_OK;
    free(s_card);
    return ESP_OK;
}

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
    ((fffs_partition_table_t *)sector_table)->last_block = 0;
    ((fffs_partition_table_t *)sector_table)->sector_size = sector_size == 0 ? 1 : sector_size;
    ((fffs_partition_table_t *)sector_table)->magic_number = FFFS_MAGIC_NUMBER;

    ((fffs_partition_table_t *)sector_table)->partition_size = partition_size == 0 ? 1 : partition_size;
    ((fffs_partition_table_t *)sector_table)->partition_id = 0;

    for (uint64_t i = 0; i < fffs_volume->sd_card->csd.capacity; i = i + (partition_size * (PARTITION_SIZE)))
    {
        fffs_erase_block(fffs_volume, i, 2);
        ESP_LOGI(TAG, "Creating Partition: %d at block number %d", ((fffs_partition_table_t *)sector_table)->partition_id, (uint32_t)i);

        memcpy(fffs_volume->read_buf, sector_table, sizeof(fffs_sector_table_t));
        FFFS_CHECK(sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, i, 1) == ESP_OK, "Cannot format sector", fail);

        ((fffs_partition_table_t *)sector_table)->partition_id++;
    }

    ESP_LOGI(TAG, "Created %d Partitions of size %d bytes.", ((fffs_partition_table_t *)sector_table)->partition_id, partition_size * (PARTITION_SIZE)*512);
    fffs_volume->current_block=1;
    err = ESP_OK;

fail:
    free(sector_table);
    return err;
}

fffs_volume_t *fffs_init(sdmmc_card_t *s_card, bool format)
{

    size_t block_size = s_card->csd.sector_size;

    fffs_volume_t *fffs_vol = malloc(sizeof(fffs_volume_t));
    FFFS_CHECK(fffs_vol, "Cannot create FFFS volume", err);

    fffs_vol->sd_card = s_card;
    fffs_vol->current_block = 0;
    fffs_vol->current_partition = 0;
    fffs_vol->current_sector = 0;
    fffs_vol->message_id = 0;
    fffs_vol->messages_in_block = 0;
    fffs_vol->block_index = 0;
    fffs_vol->partition_size = 4;
    fffs_vol->sector_size = 1;
    fffs_vol->message_rotate = false;

    fffs_vol->read_buf = heap_caps_malloc(block_size, MALLOC_CAP_DMA);
    FFFS_CHECK(fffs_vol->read_buf, "Cannot create read/write buffer for FFFS volume", err);

    ESP_LOGI(TAG, "Starting FF Filing System.");

    if (sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, 0, 1) == ESP_OK)
    {
        if ((((fffs_partition_table_t *)fffs_vol->read_buf)->magic_number) == FFFS_MAGIC_NUMBER)
        {
            ESP_LOGI(TAG, "FFS: Found Boot Partition.");

            fffs_vol->partition_size = ((fffs_partition_table_t *)fffs_vol->read_buf)->partition_size == 0 ? 1 : ((fffs_partition_table_t *)fffs_vol->read_buf)->partition_size;
            fffs_vol->sector_size = ((fffs_partition_table_t *)fffs_vol->read_buf)->sector_size == 0 ? 1 : ((fffs_partition_table_t *)fffs_vol->read_buf)->sector_size;

            ESP_LOGI(TAG, "Partitions size %d bytes.", fffs_vol->partition_size * (PARTITION_SIZE)*SD_BLOCK_SIZE);
            ESP_LOGI(TAG, "Sector size (%d) %d bytes.", fffs_vol->sector_size, fffs_vol->sector_size * (SECTOR_SIZE)*SD_BLOCK_SIZE);

            if ((((fffs_partition_table_t *)fffs_vol->read_buf)->card_full) == true)
            {
                ESP_LOGE(TAG, "SD Card is full!");
                goto fail;
            }

            while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_partition) == true)
            {
                ESP_LOGI(TAG, "Partition %d is full. Checking next partition.", fffs_vol->current_partition);
                fffs_vol->current_partition++;
                fffs_vol->current_block = fffs_vol->current_partition * (fffs_vol->partition_size * PARTITION_SIZE);
                if (fffs_vol->current_block >= fffs_vol->sd_card->csd.capacity)
                {
                    ESP_LOGE(TAG, "SD Card is full!");
                    goto fail;
                }

                FFFS_CHECK(sdmmc_read_sectors(s_card, fffs_vol->read_buf, fffs_vol->current_block, 1) == ESP_OK, "Cannot read partition.", fail);
            }

            fffs_vol->current_sector = 0;

            while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_sector) == true)
            {
                ESP_LOGI(TAG, "Sector %d in partition %d is full. Checking next sector.", fffs_vol->current_sector, fffs_vol->current_partition);
                fffs_vol->current_block = fffs_vol->current_block + (fffs_vol->sector_size * (SECTOR_SIZE));
                FFFS_CHECK(sdmmc_read_sectors(s_card, fffs_vol->read_buf, fffs_vol->current_block, 1) == ESP_OK, "Cannot read partition.", fail);
            }

            fffs_vol->current_sector = fffs_vol->current_block;
            fffs_vol->current_block = ((fffs_partition_table_t *)fffs_vol->read_buf)->last_block;
            fffs_vol->message_id = ((fffs_partition_table_t *)fffs_vol->read_buf)->message_id;

            fffs_vol->block_index = 0;
            while (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[fffs_vol->block_index + 1] > 0)
            {
                fffs_vol->block_index++;
            }
            fffs_vol->messages_in_block = ((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[fffs_vol->block_index];

            if (fffs_vol->current_block == 0)
                fffs_vol->current_block = 1;
        }
        else
        {
            if (format)
                fffs_format(fffs_vol, 2, 1, false);
        }

        return fffs_vol;
    }
    else
    {
        ESP_LOGI(TAG, "FFFS Error:  Cannot read boot sector (0) from SD card. Checking.");
    }

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

esp_err_t fffs_update_table(fffs_volume_t *fffs_volume)
{

    FFFS_CHECK(sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector, 1) == ESP_OK, "Cannot read sector ", fail);

    (((fffs_partition_table_t *)fffs_volume->read_buf)->last_block) = fffs_volume->current_block;
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
    //if(fffs_volume->message_id>0)
    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_block, 1);

    int tmp = 0, i = 0;

    //Search for the last message written to the block
    do
    {
        tmp = *((uint8_t *)(fffs_volume->read_buf) + i);

        if (tmp == 0 && *((uint8_t *)(fffs_volume->read_buf) + i + 1) > 0)
        {
            tmp = (*((uint8_t *)(fffs_volume->read_buf) + i + 1)) + 0x100;
        }

        if ((i + tmp) >= (SD_BLOCK_SIZE - 2))
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

esp_err_t print_vol_block(fffs_volume_t *fffs_volume, size_t block_num, const char *type)
{
    esp_err_t err;
    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, block_num, 1);
    if (err != ESP_OK)
        return ESP_FAIL;
    if (strcmp(type, "asc") == 0)
        print_Message2ASC(fffs_volume->read_buf, SD_BLOCK_SIZE);

    else
    {
        print_Message2HEX(fffs_volume->read_buf, SD_BLOCK_SIZE);
    }

    return ESP_OK;
}

char *fffs_read(fffs_volume_t *fffs_vol, size_t message_num)
{
    uint32_t fetch_block;
    uint8_t partition = 0;
    char *message;
    if (message_num > fffs_vol->message_id)
        return NULL;

    do
    {
        fetch_block = (fffs_vol->partition_size * (PARTITION_SIZE)) * partition++;
        sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);

    } while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_partition) == true && (((fffs_partition_table_t *)fffs_vol->read_buf)->message_id < message_num));

     --partition;
    int message_base;

    while ((((fffs_partition_table_t *)fffs_vol->read_buf)->jump_to_next_sector) == true && (((fffs_partition_table_t *)fffs_vol->read_buf)->message_id < message_num))
    {
        fetch_block = fetch_block + (SECTOR_SIZE);
        sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);
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

    sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);
    int index = 0;
    for (unsigned char offset = (message_num - old_message_base); offset > 0; offset--)
    {
        index = index + (*((uint8_t *)(fffs_vol->read_buf) + index) == 0 ? 0xFF + (*((uint8_t *)(fffs_vol->read_buf) + index + 1)) : *((uint8_t *)(fffs_vol->read_buf) + index));
    }

    print_Message2ASC(((uint8_t *)(fffs_vol->read_buf) + index + 1) + (*((uint8_t *)(fffs_vol->read_buf) + index) == 0 ? 1 : 0), (*((uint8_t *)(fffs_vol->read_buf) + index) == 0 ? 0xFF + (*((uint8_t *)(fffs_vol->read_buf) + index + 1)) : *((uint8_t *)(fffs_vol->read_buf) + index))-1);

    return message;
}

void read_blocks(fffs_volume_t *fffs_vol)
{
    for (int x = 0; x < 300; x++)
    {
        fffs_read(fffs_vol, x);
    }
}

void write_blocks(fffs_volume_t *fffs_vol)
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
    int message_base = fffs_vol->message_id;

    for (int i = 0; i < 360; i++)
    {
        message_size = 20 + i;
        if (i % 100 == 0)
            printf(".");
        fflush(stdout);
        sprintf(message, "ID:%04u..Length: %d %s", message_base++,message_size, template);
        fffs_write(fffs_vol, message, message_size);
    }

    for (int i = 0; i < fffs_vol->current_block; i = i + SECTOR_SIZE)
    {
        printf("BLOCK [%d]:\n", i);
        print_vol_block(fffs_vol, i, "hex");
        print_vol_block(fffs_vol, i + 1, "asc");
        print_vol_block(fffs_vol, i + 2, "asc");
    }
    fflush(stdout);
    //fffs_erase_block(fffs_vol, 0, 512);
    return;
}

void app_main(void)
{
    sd_card_init();

    fffs_volume_t *fffs_vol = fffs_init(s_card, true);
    if (fffs_vol == NULL)
        goto err;

    ESP_LOGI(TAG, "Current Partition ID: %d.", fffs_vol->current_partition);
    ESP_LOGI(TAG, "Current Block ID: %d.", (uint32_t)fffs_vol->current_block);
    ESP_LOGI(TAG, "Current Message ID: %d.\n", (uint32_t)fffs_vol->message_id);

    ESP_LOGI(TAG, "Current Sector ID: %d.", (uint32_t)fffs_vol->current_sector);
    ESP_LOGI(TAG, "Current Message Index ID: %d.", (uint32_t)fffs_vol->block_index);
    ESP_LOGI(TAG, "Number of Messages in Current Block ID: %d.", (uint32_t)fffs_vol->messages_in_block);

    //fffs_erase_block(fffs_vol, 0, 4096);
    ESP_LOGI(TAG, "Ready.");
    write_blocks(fffs_vol);
    read_blocks(fffs_vol);
err:
    fffs_deinit(fffs_vol);
    sd_card_deinit();
}
