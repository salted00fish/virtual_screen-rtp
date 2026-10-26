#ifndef VIDIO_H
#define VIDIO_H

#include "esp_err.h"

#pragma pack(1)
typedef struct {
    uint8_t  vpxcc;     /* V(2) P(1) X(1) CC(4) */
    uint8_t  mpt;       /* M(1) PT(7) */
    uint16_t seq;       /* 大端 */
    uint32_t ts;        /* 大端 */
    uint32_t ssrc;      /* 大端 */
} rtp_head_t;
#pragma pack()

typedef struct {
    uint32_t packets;         /* 收到的 RTP 包数 */
    uint32_t frames;          /* 成功解码并上屏的帧数 */
    uint32_t dropped;         /* 丢弃的帧数（丢包/超长/解码失败/解码来不及时） */
    uint32_t bad_packets;     /* 头部不合法、直接丢掉的包数 */
    uint32_t last_decode_us;  /* 最近一帧的解码耗时（微秒） */
    uint16_t fps;             /* 最近的上屏帧率 */
    uint32_t ssrc;            /* 最近一帧的 SSRC */
} rtp_states_t;

#define RTP_VERSION                 2
#define RTP_PT_JPEG                 26
#define RTP_PT_AUDIO                96

esp_err_t video_init(void);

void show_video_info(void);

#endif