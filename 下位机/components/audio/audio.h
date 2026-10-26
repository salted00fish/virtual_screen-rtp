#ifndef AUDIO_H
#define AUDIO_H

#include "esp_err.h"

esp_err_t audio_init(void);

/* 每秒打印一次 KCP 接收侧统计（重传统计 / 窗口占用），调试用 */
void audio_kcp_info(void);

#endif