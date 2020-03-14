#include <stdio.h>

#include "esp_err.h"
#include "fffs_utils.h"
#include <time.h>
#include <ctype.h>
#include <string.h>
#include "sdmmc_cmd.h"

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

