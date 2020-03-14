#pragma once
#ifndef _FFFS_DISK_H_
#define _FFFS_DISK_H_

#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"

sdmmc_card_t *sd_card_init();
esp_err_t sd_card_deinit(sdmmc_card_t *s_card);
#endif