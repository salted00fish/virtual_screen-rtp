#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t audio_i2s_init(void);
esp_err_t audio_i2s_write(uint8_t *pcm,uint32_t size,uint32_t *write_byte);
void *get_i2s_sema(void);
#endif