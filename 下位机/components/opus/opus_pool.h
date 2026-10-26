#ifndef OPUS_POOL_H
#define OPUS_POOL_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OPUS_POOL_BLOCK 30
#define OPUS_POOL_SIZE  1275

/* 节点状态：真实数据 / 缺包占位（等 FEC 来填） */
#define OPUS_FLAG_DATA  0
#define OPUS_FLAG_MISS  1

typedef struct opus_pool 
{
    union{
        struct opus_pool *free_next;      /* 空闲链表的 next */
        struct opus_pool *next;           /* 使用链表的 next */
    } link;

    struct opus_pool *previous;
    uint16_t seq;                         /* RTP 序号，链表按它升序 */
    uint16_t len;                         /* data 里的有效字节数 */
    uint8_t  flag;                        /* OPUS_FLAG_DATA / OPUS_FLAG_MISS */
    uint8_t  data[OPUS_POOL_SIZE];        /* 整包：RTP 头 + Opus 数据 */
}__attribute__((aligned(64))) opus_pool_t;


void opus_pool_init(void);

void opus_queue_write(uint16_t seq,const uint8_t *data,uint32_t len);


bool opus_queue_pop(opus_pool_t **out);
void opus_queue_release(opus_pool_t *node);

bool opus_fec_handle(const uint8_t *pkt,uint16_t len);

uint32_t opus_queue_size(void);

void   ops_pool_info(void);

#endif
