#pragma once
#ifndef _FFFS_UTILS_H_
#define _FFFS_UTILS_H_


#include "esp_err.h"
#include "fffs.h"

void print_Message2HEX(const unsigned char *message, size_t msgLength);

void print_Message2ASC(const unsigned char *message, size_t msgLength);

esp_err_t print_vol_block(fffs_volume_t *fffs_volume, size_t block_num, const char *type);
#endif