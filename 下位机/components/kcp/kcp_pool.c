#include "kcp_pool.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "KCP_POOL"
#define KCP_POOL_BLOCK 30
#define KCP_POOL_SIZE  1500

typedef union kcp_pool
{
    union kcp_pool *next;
    uint8_t data[KCP_POOL_SIZE];
}__attribute__((aligned(64))) kcp_pool_t;

static kcp_pool_t *base = NULL;
static kcp_pool_t *idle_pool = NULL;
static portMUX_TYPE pool_lock = portMUX_INITIALIZER_UNLOCKED;

static size_t pool_max_use = 0;
static size_t pool_use = 0;
static size_t pool_malloc_fail = 0;


void kcp_pool_init(void)
{
    if(base != NULL) return;

    base = heap_caps_aligned_calloc(64,KCP_POOL_BLOCK,sizeof(kcp_pool_t),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(base == NULL)
    {
        ESP_LOGE(TAG,"池申请失败");
        return;
    }

    for(size_t i=0;i<KCP_POOL_BLOCK - 1;i++)
    {
        base[i].next = &base[i+1];
    }

    base[KCP_POOL_BLOCK - 1].next = NULL;
    idle_pool = &base[0];
     
}

void *kcp_pool_malloc(size_t size)
{
    if(base == NULL || size > KCP_POOL_SIZE)
    {
        pool_malloc_fail++;
        return heap_caps_malloc(size,MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    }

    kcp_pool_t *temp = NULL;

    portENTER_CRITICAL(&pool_lock);

    if(idle_pool != NULL)
    {
        temp = idle_pool;
        idle_pool = temp->next;

        pool_use++;
        if(pool_use > pool_max_use)
        {
            pool_max_use = pool_use;
        }
    }
    portEXIT_CRITICAL(&pool_lock);

    if(temp == NULL)
    {
        pool_malloc_fail++;
        return heap_caps_malloc(size,MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    }

    return temp;
}

void kcp_pool_free(void *ptr)
{
    if(ptr == NULL) return;

    if(base != NULL && (uint8_t *)ptr >=(uint8_t*)base && (uint8_t *)ptr < (uint8_t*)base+KCP_POOL_BLOCK*sizeof(kcp_pool_t))
    {
        kcp_pool_t *temp = (kcp_pool_t *)ptr;

        portENTER_CRITICAL(&pool_lock);

        temp->next = idle_pool;
        idle_pool = temp;
        pool_use--;
        portEXIT_CRITICAL(&pool_lock);

        return;
    }

    heap_caps_free(ptr);
}

void kcp_pool_info(void)
{
    ESP_LOGI(TAG, "占用 %u/%d (峰值 %u) | fail %u 次",pool_use,KCP_POOL_BLOCK,pool_max_use,pool_malloc_fail);
}