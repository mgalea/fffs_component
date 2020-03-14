
#pragma once
#ifndef _FFFS_H_
#define _FFFS_H_

#include "esp_err.h"
#include "driver/sdmmc_host.h"


#define KILOBYTE 1024
#define MEGABYTE KILOBYTE * 1024
#define GIGABYTE MEGABYTE * 1024

//Default sector boundary multiples
#define PARTITION_SIZE ((256 * MEGABYTE) / SD_BLOCK_SIZE) //Expressed in block size
#define SECTOR_SIZE ((128 * KILOBYTE) / SD_BLOCK_SIZE)    //Expressed in block size
#define BLOCKS_IN_SECTOR 1                                //This is the number of blocks reported per sector 1=all blacks ina sector

#define SD_BLOCK_SIZE 512 //<DEFAULT SD CARD BLOCK size
#define FFFS_MAGIC_NUMBER 0xFFFFFFFEFDFDFBFB //< MAGIC NUMBER READS LFS001

typedef struct fffs_partition_table//__attribute__((packed))
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

}fffs_partition_table_t;

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
    uint32_t last_block;
    uint32_t block_index;
    char messages_in_block;
    uint32_t message_id;
    bool message_rotate;
}fffs_volume_t;

fffs_volume_t *fffs_init(sdmmc_card_t *s_card, bool format);

esp_err_t fffs_deinit(fffs_volume_t *fffs_vol);

esp_err_t fffs_read_block(fffs_volume_t *volume, int block_num);

esp_err_t fffs_format(fffs_volume_t *fffs_volume, unsigned char partition_size, unsigned char sector_size, bool message_rotate);

esp_err_t fffs_write(fffs_volume_t *fffs_volume, void *message, int size);

esp_err_t fffs_read(fffs_volume_t *fffs_vol, size_t message_num, uint8_t *message, int *size);

#endif