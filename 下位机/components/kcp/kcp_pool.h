#ifndef KCP_POOL_H
#define KCP_POOL_H

#include "esp_err.h"

void kcp_pool_init(void);
void *kcp_pool_malloc(size_t size);
void kcp_pool_free(void *ptr);
void kcp_pool_info(void);

#endif /* LED_H */