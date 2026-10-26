#include "opus_pool.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "OPUS_POOL"

#define OPUS_HOLD_FRAMES   6        /* 队头落后最新 6 帧（60ms）才兜底放行：FEC 正常 10ms 内就到 */
#define OPUS_HOLD_MS       50       /* 上游断流的兜底：超时就把队头放走 */
#define OPUS_MAX_GAP       12       /* 一次最多补多少个占位，防上位机暂停后爆池 */

#define FEC_GROUP          3
#define FEC_HEAD_LEN       (3 + 2 + 2 * FEC_GROUP)     /* "FEC" + seq(2) + len[3](6) = 11 */

static opus_pool_t *base      = NULL;
static opus_pool_t *idle_pool = NULL;
static opus_pool_t *opus_head = NULL;
static opus_pool_t *opus_tail = NULL;

static SemaphoreHandle_t opus_mutex = NULL;

static uint16_t newest_seq    = 0;
static bool     seq_init      = false;
static uint32_t last_write_ms = 0;

/* 已经完整收到 FEC 的最后一个 seq：这个序号之前的帧，FEC 已经给过结论了 */
static uint16_t fec_done_seq  = 0;
static bool     fec_done_init = false;

static size_t pool_use         = 0;
static size_t pool_max_use     = 0;
static size_t pool_malloc_fail = 0;
static size_t opus_write_cnt   = 0;
static size_t opus_read_cnt    = 0;
static size_t miss_cnt_total   = 0;
static size_t fec_try_cnt      = 0;
static size_t fec_ok_cnt       = 0;

/* FEC 失败分类计数 */
static size_t f_no_loss   = 0;   /* 组内没缺包 */
static size_t f_multi     = 0;   /* 组内缺 2 帧以上 */
static size_t f_lenbad    = 0;   /* 本地包长和 FEC 记录的不一致 */
static size_t f_notfound  = 0;   /* 节点已被消费，救不回来 */
static size_t f_short     = 0;   /* FEC 包太短 / need 越界 */


static inline int16_t seq_diff(uint16_t a,uint16_t b)
{
    return (int16_t)((uint16_t)(a - b));
}

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/**这一帧所在的FEC组是否已经给过结论(组尾被FEC覆盖过)*/
static bool fec_group_past(uint16_t seq)
{
    if(!fec_done_init) return false;

    uint16_t group_end = (uint16_t)(seq + FEC_GROUP - 1);

    return seq_diff(fec_done_seq,group_end) >= 0;
}

/**记录 FEC 已经覆盖到的组尾 */
static void fec_mark_done(uint16_t seq0)
{
    uint16_t end = (uint16_t)(seq0 + FEC_GROUP - 1);

    if(!fec_done_init || seq_diff(end,fec_done_seq) > 0)
    {
        fec_done_seq  = end;
        fec_done_init = true;
    }
}

static void lock(void)
{
    if(opus_mutex) (void)xSemaphoreTake(opus_mutex,portMAX_DELAY);
}

static void unlock(void)
{
    if(opus_mutex) xSemaphoreGive(opus_mutex);
}

void opus_pool_init(void)
{
    if(base != NULL) return;

    opus_mutex = xSemaphoreCreateMutex();
    if(opus_mutex == NULL)
    {
        ESP_LOGE(TAG,"互斥锁创建失败");
        return;
    }

    base = heap_caps_aligned_calloc(64,OPUS_POOL_BLOCK,sizeof(opus_pool_t),MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(base == NULL)
    {
        ESP_LOGE(TAG,"池申请失败");
        return;
    }

    for(size_t i=0;i<OPUS_POOL_BLOCK - 1;i++)
    {
        base[i].link.free_next = &base[i+1];
        base[i].previous       = NULL;
    }

    base[OPUS_POOL_BLOCK - 1].link.free_next = NULL;
    base[OPUS_POOL_BLOCK - 1].previous       = NULL;

    idle_pool = &base[0];
}

static opus_pool_t *pool_alloc_locked(void)
{
    if(idle_pool == NULL)
    {
        pool_malloc_fail++;
        return NULL;
    }

    opus_pool_t *node = idle_pool;
    idle_pool = node->link.free_next;

    node->link.next = NULL;
    node->previous  = NULL;
    node->seq       = 0;
    node->len       = 0;
    node->flag      = OPUS_FLAG_MISS;

    pool_use++;
    if(pool_use > pool_max_use) pool_max_use = pool_use;

    return node;
}

static void pool_free_locked(opus_pool_t *node)
{
    node->previous       = NULL;
    node->link.free_next = idle_pool;
    idle_pool            = node;

    if(pool_use)
        pool_use--;
}

//按 seq 升序插入
static void list_insert_locked(opus_pool_t *node)
{
    opus_pool_t *cur = opus_tail;

    while(cur && seq_diff(node->seq,cur->seq) < 0)
    {
        cur = cur->previous;
    }

    if(cur == NULL)
    {
        node->previous  = NULL;
        node->link.next = opus_head;

        if(opus_head)
            opus_head->previous = node;

        opus_head = node;

        if(opus_tail == NULL)
            opus_tail = node;
    }
    else
    {
        node->previous  = cur;
        node->link.next = cur->link.next;

        if(cur->link.next)
            cur->link.next->previous = node;
        else
            opus_tail = node;

        cur->link.next = node;
    }

    if(seq_init && seq_diff(node->seq,newest_seq) > 0)
    {
        newest_seq = node->seq;
    }
}

static opus_pool_t *find_locked(uint16_t seq)
{
    opus_pool_t *cur = opus_head;

    while(cur)
    {
        if(cur->seq == seq)
            return cur;

        cur = cur->link.next;
    }

    return NULL;
}

static void clear_locked(void)
{
    opus_pool_t *cur = opus_head;

    while(cur)
    {
        opus_pool_t *next = cur->link.next;
        pool_free_locked(cur);
        cur = next;
    }

    opus_head     = NULL;
    opus_tail     = NULL;
    seq_init      = false;
    fec_done_init = false;
}

void opus_queue_write(uint16_t seq,const uint8_t *data,uint32_t len)
{
    if(data == NULL || len == 0 || len > OPUS_POOL_SIZE || opus_mutex == NULL)
    {
        return;
    }

    lock();

    opus_pool_t *node = find_locked(seq);

    if(node)
    {
        /* FEC 先还原过这一帧 → 把占位换成真实数据；否则就是重复包，丢掉 */
        if(node->flag == OPUS_FLAG_MISS)
        {
            memcpy(node->data,data,len);
            node->len  = (uint16_t)len;
            node->flag = OPUS_FLAG_DATA;
        }

        unlock();
        return;
    }

    if(seq_init && seq_diff(seq,newest_seq) <= 0)
    {
        /**也可能没内存了，分配不到序号，网络乱序又是传播 没内存，序号又是旧的，该丢就丢了*/
        unlock();
        return;
    }

    if(seq_init)
    {
        int16_t gap = (int16_t)(seq_diff(seq,newest_seq) - 1);

        if(gap > OPUS_MAX_GAP)
        {
            /* 上位机暂停过、seq 跳了一大段：整条链作废，从这一包重新开始 */
            clear_locked();
            gap = 0;
        }

        uint16_t base = (uint16_t)(newest_seq + 1);

        /**丢包的时候，才会进行占位，防止数据包乱序到达 */
        for(int16_t i=0;i<gap;i++)
        {
            opus_pool_t *miss = pool_alloc_locked();

            if(miss == NULL)break;

            miss->seq  = (uint16_t)(base + (uint16_t)i);
            miss->len  = 0;
            miss->flag = OPUS_FLAG_MISS;

            list_insert_locked(miss);
            miss_cnt_total++;
        }
    }

    node = pool_alloc_locked();

    if(node)
    {
        node->seq  = seq;
        node->len  = (uint16_t)len;
        node->flag = OPUS_FLAG_DATA;

        memcpy(node->data,data,len);
        list_insert_locked(node);
    }

    newest_seq    = seq;
    seq_init      = true;
    last_write_ms = now_ms();
    opus_write_cnt++;

    unlock();
}

bool opus_queue_pop(opus_pool_t **out)
{
    if(out == NULL || opus_mutex == NULL) return false;

    lock();

    if(opus_head == NULL)
    {
        unlock();
        return false;
    }

    /* 优先看 FEC 有没有给这一组下定论；没有就退回按帧数/超时判断 */
    bool ready = fec_group_past(opus_head->seq);

    if(!ready)
    {
        int16_t  lag  = seq_diff(newest_seq,opus_head->seq);
        uint32_t idle = now_ms() - last_write_ms;

        /* 队头离最新帧不到 3 帧，且上游没断流 → 先攒着，等 FEC 把它填上 */
        ready = (lag >= OPUS_HOLD_FRAMES) || (idle >= OPUS_HOLD_MS);
    }

    if(!ready)
    {
        unlock();
        return false;
    }

    opus_pool_t *node = opus_head;

    opus_head = node->link.next;
    if(opus_head)
        opus_head->previous = NULL;
    else          
        opus_tail = NULL;

    node->link.next = NULL;
    node->previous  = NULL;

    opus_read_cnt++;
    unlock();

    *out = node;
    return true;
}

void opus_queue_release(opus_pool_t *node)
{
    if(node == NULL || opus_mutex == NULL) return;

    lock();
    pool_free_locked(node);
    unlock();
}

bool opus_fec_handle(const uint8_t *pkt,uint16_t len)
{
    if(pkt == NULL || len < FEC_HEAD_LEN || opus_mutex == NULL) return false;

    uint16_t seq0 = (uint16_t)((pkt[3] << 8) | pkt[4]);
    uint16_t plen[FEC_GROUP];


    for(int i=0;i<FEC_GROUP;i++)
    {

        const uint8_t *p = pkt + 5 + i*2;
        
        plen[i] = (uint16_t)((p[0] << 8) | p[1]);

    }

    const uint8_t *xor_data = pkt + FEC_HEAD_LEN;
    uint16_t       xor_len  = (uint16_t)(len - FEC_HEAD_LEN);

    opus_pool_t *node[FEC_GROUP] = {0};

    int  miss     = -1;
    int  miss_cnt = 0;
    bool ok       = false;

    lock();

    fec_try_cnt++;

    for(int i=0;i<FEC_GROUP;i++)
    {
        uint16_t s = (uint16_t)(seq0 + i);

        node[i] = find_locked(s);

        
        if(node[i] == NULL)
        {
            /**包里找不到fec数据起始的序号 */
            if(!seq_init || seq_diff(s,newest_seq) <= 0)
            {
                /**找不到fec保护的序号，又比最新的序号小，丢了 */
                f_notfound++;
                goto done;
            }

            node[i] = pool_alloc_locked();
            if(node[i] == NULL)
            {
                f_short++;
                goto done;
            }

            node[i]->seq  = s;
            node[i]->len  = 0;
            node[i]->flag = OPUS_FLAG_MISS;

            list_insert_locked(node[i]);
        }

        if(node[i]->flag == OPUS_FLAG_MISS)
        {
            miss = i;
            miss_cnt++;
        }
        else if(node[i]->len != plen[i])
        {
            f_lenbad++;
            goto done;      /* 长度对不上，两端口径不一致，不敢用 */
        }
    }

    if(miss_cnt == 0)
    {
        f_no_loss++;
        goto done;
    }

    if(miss_cnt >= 2)
    {
        f_multi++;
        goto done;
    }

    
    uint16_t need = plen[miss];

    if(need == 0 || need > OPUS_POOL_SIZE || need > xor_len)
    {
        f_short++;
        goto done;
    }

    uint8_t *dst = node[miss]->data;

    memcpy(dst,xor_data,need);

    for(int i=0;i<FEC_GROUP;i++)
    {
        if(i == miss) continue;

        uint16_t n = (plen[i] < need) ? plen[i] : need;

        for(uint16_t j=0;j<n;j++)
        {
            dst[j] ^= node[i]->data[j];
        }
    }

    node[miss]->len  = need;
    node[miss]->flag = OPUS_FLAG_DATA;

    fec_ok_cnt++;
    ok = true;
    

done:
    /* 不管救没救回来，这一组的 FEC 已经到过，后面不会再补了 */
    fec_mark_done(seq0);

    unlock();
    return ok;
}


uint32_t opus_queue_size(void)
{
    uint32_t n = 0;

    lock();
    opus_pool_t *cur = opus_head;

    while(cur)
    {
        n++;
        cur = cur->link.next;
    }

    unlock();

    return n;
}

void ops_pool_info(void)
{
    size_t use, max_use, wr, rd, miss, fec_ok, fec_try, fail;
    size_t n_loss, n_multi, n_lenbad, n_notfound, n_short;

    lock();

    use     = pool_use;
    max_use = pool_max_use;
    wr      = opus_write_cnt;
    rd      = opus_read_cnt;
    miss    = miss_cnt_total;
    fec_ok  = fec_ok_cnt;
    fec_try = fec_try_cnt;
    fail    = pool_malloc_fail;

    n_loss     = f_no_loss;
    n_multi    = f_multi;
    n_lenbad   = f_lenbad;
    n_notfound = f_notfound;
    n_short    = f_short;


    unlock();

    ESP_LOGI(TAG,"占用 %u/%d 峰值 %u | 写 %u 读 %u miss %u | FEC %u/%u | fail %u",
             (unsigned)use,OPUS_POOL_BLOCK,(unsigned)max_use,
             (unsigned)wr,(unsigned)rd,(unsigned)miss,
             (unsigned)fec_ok,(unsigned)fec_try,(unsigned)fail);

    ESP_LOGI(TAG,"FEC失败: 无缺 %u 多缺 %u 长度 %u 找不到 %u 太短 %u",
             (unsigned)n_loss,(unsigned)n_multi,(unsigned)n_lenbad,
             (unsigned)n_notfound,(unsigned)n_short);
}
