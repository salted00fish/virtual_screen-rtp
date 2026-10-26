#ifndef __AUDIO_CONFIG_H
#define __AUDIO_CONFIG_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "ikcp.h"

#define AUDIO_PIN_DOUT          GPIO_NUM_50
#define AUDIO_PIN_BCLK          GPIO_NUM_51
#define AUDIO_PIN_WS            GPIO_NUM_52
#define AUDIO_PIN_SD            GPIO_NUM_49


/* ============================ 音频参数 ============================ */
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2
#define AUDIO_FRAME_BYTES       (2 * AUDIO_CHANNELS)
#define AUDIO_ADJ_FRAMES        (AUDIO_SAMPLE_RATE / 1000)              /* 1ms */

#define AUDIO_RING_FRAMES       (AUDIO_SAMPLE_RATE * 320 / 1000)
#define AUDIO_ONE_FRAMES        ((AUDIO_SAMPLE_RATE * 10 / 1000))

#define AUDIO_DMA_DESC_NUM      6
#define AUDIO_DMA_FRAME_NUM     480      /* 6*256/48000 ≈ 32ms DMA 缓冲 */

typedef struct
{
    uint32_t write;
    uint32_t read;
    uint8_t *audio_queue;
    uint8_t *i2s_buf;
    portMUX_TYPE spinlock;

    uint8_t *opus_recv_buf[3];
    uint8_t *opus_recv_front;
    uint8_t *opus_recv_ready;
    uint8_t *opus_recv_back;
    uint32_t opus_recv_ready_len;
    uint8_t ready_used;
    ikcpcb *audio_kcp;
    SemaphoreHandle_t kcp_audio_mutex;
}audio_ops_t;

#endif