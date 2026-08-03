#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include "port_sdcard.h"
#include "epaper_config.h"

sdmmc_card_t *card = NULL;  
uint32_t sdcard_slot = 0;


bool Sdcard_Init(void)
{
  	esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;     //If the hook fails, create a partition table and format the SD car
    mount_config.max_files = 5;                      //Maximum number of open files
    mount_config.allocation_unit_size = 2 * 1024;         //Similar to sector size

  	sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  	host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;//high speed

  	sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  	slot_config.width = 1;           //1-wire
  	slot_config.clk = SD_CLK_PIN;
  	slot_config.cmd = SD_MOSI_CMD_PIN;
  	slot_config.d0 = SD_MISO_D0_PIN;
  	ESP_ERROR_CHECK_WITHOUT_ABORT(esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card));

  	if(card != NULL) {
  	  	sdmmc_card_print_info(stdout, card);
		sdcard_slot = 1;
		return 1;
	}
	return 0;
}

float sd_card_get_value(void) {
  	if(card != NULL) {
  	  	return (float)(card->csd.capacity)/2048/1024; 
  	}
  	return -1;
}

