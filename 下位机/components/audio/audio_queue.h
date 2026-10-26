#ifndef AUDIO_QUEUE_H
#define AUDIO_QUEUE_H

#include "esp_err.h"
#include "audio_config.h"

esp_err_t audio_queue_init(uint8_t **audio_queue,uint8_t **i2s_buf);
uint32_t audio_queue_use(audio_ops_t *ops);
uint32_t audio_queue_read(audio_ops_t *ops,uint16_t *dst, uint32_t want);
bool audio_queue_push(audio_ops_t *ops,uint8_t *pcm,uint32_t frame,uint32_t pts_ms);
void audio_switch_buf(audio_ops_t *ops);
#endif