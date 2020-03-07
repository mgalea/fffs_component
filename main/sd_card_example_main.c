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

static const char *TAG = "sd_card";

#define USE_SPI_MODE
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 4

#define KILOBYTE 1024
#define MEGABYTE KILOBYTE * 1024
#define GIGABYTE MEGABYTE * 1024
#define BLOCK_SIZE 512
#define SECTOR_SIZE 256
#define BLOCKS_IN_SECTOR 1 //This is the number of blocks reported per sector 1=all blacks ina sector

#define APP_CHECK(a, str, goto_tag, ...)                                          \
    do                                                                            \
    {                                                                             \
        if (!(a))                                                                 \
        {                                                                         \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                        \
        }                                                                         \
    } while (0)

#define FFFS_BOOT_SECTOR()                 \
    {                                      \
        .magic_number = FFFS_MAGIC_NUMBER, \
        .jump_to_next_partition = 0,       \
        .jump_to_next_sector = 0,          \
        .partition_id = 0,                 \
        .sector_id = 0,                    \
        .last_block = 1,                   \
    }

#define FFFS_VOLUME_DEFAULT() \
    {                         \
        .current_block = 0,   \
        .current_sector = 0,  \
        .jump_partition = 0,  \
        .head = NULL,         \
    }

//.partition_size = 1024 * 1024 * 2,
// .sector_size = 1024 * 64,

#define FFFS_MAGIC_NUMBER 0xFFFFFFFEFDFDFBFB //< MAGIC NUMBER READS LFS001

static sdmmc_card_t *s_card = NULL;

typedef struct //__attribute__((packed))
{
    unsigned int jump_to_next_partition : 2; //<If value is 0 then this is the current partition - Each partition is 1 Gigabyte (1024*1024*2) sectors in size or PARTITION_SIZE
    unsigned int jump_to_next_sector : 2;    //< If value is 0x0 then this is the current sector otherwise goto next sector - A sector is 1024M/10;
    unsigned int flags : 4;
    char partition_id;
    uint32_t last_block; //< If above value is 0x0 then this value points to the last block written in the partition;
    uint64_t magic_number;
    uint32_t message_id;
} fffs_partition_t;

typedef struct
{
    sdmmc_card_t *sd_card;
    void *read_buf;
    uint32_t current_block;
    uint32_t current_sector_block;
    uint32_t current_sector_table_block;
    char current_partition;
    char messages_in_block;
    uint32_t message_id;
} fffs_volume_t;

typedef struct //__attribute__((packed))
{
    fffs_partition_t boot_partition;
    //uint32_t sector_message_base_index;
    uint8_t sector_message_index[SECTOR_SIZE / BLOCKS_IN_SECTOR];
} fffs_sector_table_t;

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

static esp_err_t fffs_erase_block(fffs_volume_t *fffs_volume, size_t block_num)
{
    esp_err_t err = ESP_OK;
    for (int i = 0; i < fffs_volume->sd_card->csd.sector_size; i++)
        *((char *)fffs_volume->read_buf + i) = 0;

    return sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, block_num, 1) == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t fffs_format(fffs_volume_t *fffs_volume)
{
    esp_err_t err = ESP_OK;
    fffs_sector_table_t *sector_table = calloc(1, sizeof(fffs_sector_table_t));

    sector_table->boot_partition.magic_number = FFFS_MAGIC_NUMBER;
    sector_table->boot_partition.jump_to_next_partition = 0;
    sector_table->boot_partition.jump_to_next_sector = 0;
    sector_table->boot_partition.last_block = 1;
    sector_table->boot_partition.message_id = 0;

    int partition = 0;
    for (uint64_t i = 0; i < fffs_volume->sd_card->csd.capacity; i = i + GIGABYTE / BLOCK_SIZE)
    {
        fffs_erase_block(fffs_volume, i);
        fffs_erase_block(fffs_volume, i + 1);
        ESP_LOGI(TAG, "Creating Partion: %d", partition);
        sector_table->boot_partition.partition_id = partition++;
        //sector_table->sector_message_base_index = 0;
        memcpy(fffs_volume->read_buf, sector_table, sizeof(fffs_sector_table_t));
        err = sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, i, 1);
        if (err != ESP_OK)
            return ESP_FAIL;
    }
    free(sector_table);

    ESP_LOGI(TAG, "FFFS: Format Success. Total number of partitions created: %d", partition - 1);
    return ESP_OK;
}

fffs_volume_t *fffs_init(sdmmc_card_t *s_card, bool format)
{
    esp_err_t err = ESP_OK;
    size_t block_size = s_card->csd.sector_size;

    fffs_volume_t *fffs_vol = malloc(sizeof(fffs_volume_t));
    if (fffs_vol == NULL)
    {
        ESP_LOGE(TAG, "Cannot create FFFS volume");
        return NULL;
    }

    fffs_vol->current_block = 1;
    fffs_vol->current_partition = 0;
    fffs_vol->current_sector_block = 0;
    fffs_vol->sd_card = s_card;
    fffs_vol->message_id = 0;
    fffs_vol->messages_in_block = 0;
    fffs_vol->current_sector_table_block = 0;

    fffs_vol->read_buf = heap_caps_malloc(block_size, MALLOC_CAP_DMA);
    if (fffs_vol->read_buf == NULL)
    {
        ESP_LOGE(TAG, "Cannot create read/write buffer for FFFS volume");
        goto fail;
    }

    ESP_LOGI(TAG, "Starting FF Filing System.");

    err = sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, 0, 1);
    if (err == ESP_OK)
    {
        if ((((fffs_partition_t *)fffs_vol->read_buf)->magic_number) == FFFS_MAGIC_NUMBER)
        {
            ESP_LOGI(TAG, "FFS: Found Boot Partition.");
            fffs_vol->current_partition = (((fffs_partition_t *)fffs_vol->read_buf)->partition_id);

            while ((((fffs_partition_t *)fffs_vol->read_buf)->jump_to_next_partition) == true)
            {
                ESP_LOGI(TAG, "Partition %d is full. Checking next partition.", fffs_vol->current_partition);
                fffs_vol->current_partition++;
                fffs_vol->current_block = fffs_vol->current_partition * GIGABYTE / BLOCK_SIZE;
                if (fffs_vol->current_block >= fffs_vol->sd_card->csd.capacity)
                {
                    ESP_LOGE(TAG, "SD Card is full!");
                    goto fail;
                }
                err = sdmmc_read_sectors(s_card, fffs_vol->read_buf, fffs_vol->current_block, 1);
                if (err != ESP_OK)
                    goto fail;
            }

            fffs_vol->current_sector_block = (((fffs_partition_t *)fffs_vol->read_buf)->last_block);
            err = sdmmc_read_sectors(s_card, fffs_vol->read_buf, fffs_vol->current_sector_block, 1);
            if (err != ESP_OK)
                goto fail;

            fffs_vol->current_block = ((fffs_partition_t *)fffs_vol->read_buf)->last_block;
            fffs_vol->message_id = ((fffs_partition_t *)fffs_vol->read_buf)->message_id;
        }
        else
        {
            if (format)
                fffs_format(fffs_vol);
            fffs_create_sector_block(fffs_vol);
            fffs_erase_block(fffs_vol, fffs_vol->current_block);
        }

        return fffs_vol;
    }
    else
    {
        ESP_LOGI(TAG, "FFFS Error: %s, Cannot read boot sector (0) from SD card. Checking.", esp_err_to_name(err));
    }

fail:
    free(fffs_vol);
    err = ESP_FAIL;

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

esp_err_t fffs_update_sector_table(fffs_volume_t *fffs_volume)
{
    esp_err_t err = ESP_FAIL;
    /*
    int total_offset = 0;

    err = sdmmc_read_sectors(s_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);

    int i = 0;
    while (((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[i] > 0)
    {
        total_offset = total_offset + ((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[i++];
    }

    
    //(((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[i]) = fffs_volume->message_id - (((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_base_index) - total_offset+1;

    //(((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[fffs_volume->current_sector_table_block]) = fffs_volume->messages_in_block;

    sdmmc_write_sectors(s_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);
    */
    fffs_volume->current_sector_table_block++;
    return ESP_OK;
}

static esp_err_t fffs_create_sector_block(fffs_volume_t *fffs_volume)
{
    esp_err_t err = ESP_FAIL;

    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_partition, 1);

    (((fffs_partition_t *)fffs_volume->read_buf)->jump_to_next_sector) = (fffs_volume->current_block != 1) ? true : false;
    (((fffs_partition_t *)fffs_volume->read_buf)->last_block) = fffs_volume->current_block;
    (((fffs_partition_t *)fffs_volume->read_buf)->magic_number) = FFFS_MAGIC_NUMBER;

    sdmmc_write_sectors(s_card, fffs_volume->read_buf, fffs_volume->current_partition, 1);
    if (err != ESP_OK)
        return ESP_FAIL;

    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);
    (((fffs_partition_t *)fffs_volume->read_buf)->jump_to_next_sector) = true;

    sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);

    fffs_volume->current_sector_block = fffs_volume->current_block;

    fffs_volume->current_sector_table_block = 0;

    //(((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_base_index) = fffs_volume->message_id;

    for (int i = 0; i < (SECTOR_SIZE / BLOCKS_IN_SECTOR); i++)
    {
        ((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[i] = 0;
    }

    (((fffs_partition_t *)fffs_volume->read_buf)->jump_to_next_sector) = false;

    sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);

    printf("Size of Struct: %d\n", sizeof(fffs_sector_table_t));

    return err != ESP_OK ? ESP_FAIL : ESP_OK;
}

esp_err_t fffs_update_table(fffs_volume_t *fffs_volume)
{
    esp_err_t err = ESP_FAIL;

    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);
    if (err != ESP_OK)
        return ESP_FAIL;

    (((fffs_partition_t *)fffs_volume->read_buf)->last_block) = fffs_volume->current_block;
    (((fffs_partition_t *)fffs_volume->read_buf)->message_id) = fffs_volume->message_id;
    (((fffs_sector_table_t *)fffs_volume->read_buf)->sector_message_index[fffs_volume->current_sector_table_block]) = fffs_volume->messages_in_block;
    sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_sector_block, 1);
    if (err != ESP_OK)
        return ESP_FAIL;

    return ESP_OK;
}

static esp_err_t fffs_next_block(fffs_volume_t *fffs_volume)
{
    esp_err_t err;

    fffs_volume->current_block++;

    err = fffs_erase_block(fffs_volume, fffs_volume->current_block);

    if (fffs_volume->current_block % SECTOR_SIZE == 0)
    {
        fffs_create_sector_block(fffs_volume);
        fffs_next_block(fffs_volume);
    }

    if (fffs_volume->current_block % BLOCKS_IN_SECTOR == 0)
    {
        //fffs_update_sector_table(fffs_volume);
        fffs_volume->messages_in_block = 0;
        fffs_volume->current_sector_table_block++;
    }

    fffs_update_table(fffs_volume);

    return fffs_volume->current_block == fffs_volume->sd_card->csd.capacity - 1 ? ESP_FAIL : ESP_OK;
}

esp_err_t sd_write_message(fffs_volume_t *fffs_volume, void *message, int size)
{
    esp_err_t err = ESP_OK;
    if (size > BLOCK_SIZE - 2 || size == 0) //messages can only 510 bytes long  since the first two bytes must be reserved for the next message offset
        return ESP_ERR_INVALID_SIZE;

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

        if ((i + tmp) >= (BLOCK_SIZE - 2))
        {
            tmp = 0;
        }
        else
        {
            i = i + tmp;
        }
    } while (tmp > 0);

    if ((BLOCK_SIZE - 2 - (i)) < size)
    {
        if (fffs_next_block(fffs_volume) == ESP_FAIL)
            return ESP_FAIL;

        return sd_write_message(fffs_volume, message, size) == ESP_OK ? ESP_OK : ESP_FAIL;
    }
    if (size < 256)
    {
        *((uint8_t *)(fffs_volume->read_buf) + i) = (uint8_t)size + 1; //this is the offset not message size
        memcpy((uint8_t *)(fffs_volume->read_buf) + i + 1, message, size);
    }
    else
    {
        //printf("Message %d is %d bytes long.\n", fffs_volume->message_id, size);
        memcpy((uint8_t *)(fffs_volume->read_buf) + i + 2, message, size);
        *((uint8_t *)(fffs_volume->read_buf) + i) = 0;                                //indicate that the message is longer than 255 characters
        *((uint8_t *)(fffs_volume->read_buf) + i + 1) = (uint8_t)((size - 0xff)) + 1; //this is the offset not message size
    }

    err = sdmmc_write_sectors(fffs_volume->sd_card, fffs_volume->read_buf, fffs_volume->current_block, 1);
    if (err != ESP_OK)
        return ESP_FAIL;
    fffs_volume->messages_in_block++;
    fffs_update_table(fffs_volume);

    fffs_volume->message_id++;

    return ESP_OK;
}

esp_err_t print_vol_block(fffs_volume_t *fffs_volume, size_t block_num, const char *type)
{
    esp_err_t err;
    err = sdmmc_read_sectors(fffs_volume->sd_card, fffs_volume->read_buf, block_num, 1);
    if (err != ESP_OK)
        return ESP_FAIL;
    if (strcmp(type, "asc") == 0)
        print_Message2ASC(fffs_volume->read_buf, BLOCK_SIZE);

    else
    {
        print_Message2HEX(fffs_volume->read_buf, BLOCK_SIZE);
    }

    return ESP_OK;
}

char *fffs_load_message(fffs_volume_t *fffs_vol, size_t message_num)
{
    ESP_LOGI(TAG, "FETCHING MESSAGE:: %d", message_num);
    uint32_t fetch_block, sector_block = 0;
    uint8_t partition = 0;
    char *message;
    if (message_num > fffs_vol->message_id)
        return NULL;

    do
    {
        fetch_block = partition++ * GIGABYTE / BLOCK_SIZE;
        sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);
    } while ((((fffs_partition_t *)fffs_vol->read_buf)->jump_to_next_partition) == true && (((fffs_partition_t *)fffs_vol->read_buf)->message_id < message_num));

    ESP_LOGI(TAG, "MESSAGE IS IN PARTITION: %d", partition - 1);
    int message_base;
    do
    {
        message_base = (((fffs_partition_t *)fffs_vol->read_buf)->message_id);

        fetch_block = sector_block * SECTOR_SIZE;
        sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);

    } while ((((fffs_partition_t *)fffs_vol->read_buf)->jump_to_next_sector) == true && (((fffs_partition_t *)fffs_vol->read_buf)->message_id < message_num) && sector_block++ < fffs_vol->current_sector_block);

    ESP_LOGI(TAG, "MESSAGE IS IN SECTOR: %d, SECTOR BLOCK: %d", sector_block, fetch_block);
    ESP_LOGI(TAG, "BASE MESSAGE IN SECTOR BLOCK: %d", message_base);
    print_Message2HEX(fffs_vol->read_buf, 512);

    fetch_block++;

    int old_message_base;

    int i = 0;
    do
    {
        old_message_base = message_base;
        message_base = message_base + (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[i++]);

    } while (message_base < ((message_num) + 1) && (((fffs_sector_table_t *)fffs_vol->read_buf)->sector_message_index[i]) != 0);

    fetch_block = fetch_block + (i * BLOCKS_IN_SECTOR) - 1;

    ESP_LOGI(TAG, "CLOSEST BLOCK: %d with message %d", fetch_block, old_message_base);
    ESP_LOGI(TAG, "MESSAGE OFFSET: %d", message_num - old_message_base);

    sdmmc_read_sectors(fffs_vol->sd_card, fffs_vol->read_buf, fetch_block, 1);
    print_Message2ASC(fffs_vol->read_buf, 512);
    /*
    uint8_t index = 0;
    do
    {
        index = *((uint8_t *)(fffs_vol->read_buf) + index);
    } while (message_num - (++old_message_base) > 0);

    print_Message2ASC(((uint8_t *)(fffs_vol->read_buf) + index), 10);
*/
    return message;
}

void read_blocks(fffs_volume_t *fffs_vol)
{
    for (int x = 0; x < 900; x = x + 50)
        fffs_load_message(fffs_vol, x);
}

void write_blocks(fffs_volume_t *fffs_vol)
{
    char template[] = "\
Had my friends Muse grown with this growing age \
He would be perjured, murderous, bloody and full of blame,\
The ersthwhile Drugs poisoned him that he fell sick of the world.\
To say, within thine own deep sunken eyes, I have no fault in this,\
Who even but now come back again, assured!";

    srand(esp_timer_get_time());

    char message[400];
    int message_size;

    for (int i = 0; i < 4000; i++)
    {
        message_size = 7 + (rand()) % 280;

        if (i % 100 == 0)
            printf(".");
        fflush(stdout);
        sprintf(message, "ID:%4u.%s", i, template);
        sd_write_message(fffs_vol, message, message_size);
    }

    for (int i = 0; i < SECTOR_SIZE * 6; i = i + SECTOR_SIZE)
    {
        printf("BLOCK [%d]:\n", i);

        print_vol_block(fffs_vol, i, "hex");
        print_vol_block(fffs_vol, i + 1, "asc");
        print_vol_block(fffs_vol, i + 2, "asc");
    }
    fflush(stdout);
    fffs_erase_block(fffs_vol, 0);
    return;
}

void app_main(void)
{
    sd_card_init();

    fffs_volume_t *fffs_vol = fffs_init(s_card, true);
    if (fffs_vol == NULL)
        goto err;

    ESP_LOGI(TAG, "Current Partition ID: %d.", fffs_vol->current_partition);
    ESP_LOGI(TAG, "Current Sector ID: %d.", (uint32_t)fffs_vol->current_sector_block);
    ESP_LOGI(TAG, "Current Block ID: %d.", (uint32_t)fffs_vol->current_block);

    write_blocks(fffs_vol);
err:
    fffs_deinit(fffs_vol);
    sd_card_deinit();
}
