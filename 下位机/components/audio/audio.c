#include "audio.h"
#include "audio_i2s.h"
#include "opus.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "udp.h"
#include "wifi.h"
#include "lwip/api.h"
#include "audio_queue.h"
#include "video.h"
#include "opus.h"
#include "ikcp.h"
#include "kcp_pool.h"
#include "opus_pool.h"
#include "touch.h"

#define TAG "audio"

void audio_kcp_task(void *parm);
TaskHandle_t audio_kcp_task_handle = NULL;

void audio_kcp_rx_task(void *parm);
TaskHandle_t audio_kcp_rx_task_handle = NULL;

void audio_rx_task(void *parm);
TaskHandle_t audio_rx_task_handle = NULL;

void opus_decode_task(void *parm);
TaskHandle_t opus_decode_task_handle = NULL;

void audio_task(void *parm);
TaskHandle_t audio_task_handle = NULL;

static QueueHandle_t      audio_eth_rx_queue = NULL;
static QueueHandle_t      audio_eth_kcp_rx_queue = NULL;
static EventGroupHandle_t wifi_group  = NULL;


audio_ops_t audio_ops = {
    .read = 0,
    .write = 0,
    .audio_queue = NULL,
    .i2s_buf = NULL,
    .spinlock = portMUX_INITIALIZER_UNLOCKED,
    .audio_kcp = NULL,
};



esp_err_t audio_init(void)
{
    esp_err_t err = ESP_OK;
    
    err = audio_queue_init(&audio_ops.audio_queue,&audio_ops.i2s_buf);
    if(err != ESP_OK)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:audio_queue_init error");
        goto error;
    }

    audio_ops.opus_recv_buf[0] = heap_caps_malloc(1024,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    audio_ops.opus_recv_buf[1] = heap_caps_malloc(1024,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    audio_ops.opus_recv_buf[2] = heap_caps_malloc(1024,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(!audio_ops.opus_recv_buf[0] || !audio_ops.opus_recv_buf[1] || !audio_ops.opus_recv_buf[2])
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:opus_recv_buf create error");
        goto error;
    }
    
    audio_ops.opus_recv_front = audio_ops.opus_recv_buf[0];
    audio_ops.opus_recv_ready = audio_ops.opus_recv_buf[1];
    audio_ops.opus_recv_back  = audio_ops.opus_recv_buf[2];

    audio_eth_rx_queue = xQueueCreate(16, sizeof(int));
    if(audio_eth_rx_queue == NULL)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:audio_queue create error");
        goto error;
    }

    audio_eth_kcp_rx_queue = xQueueCreate(5, sizeof(int));
    if(audio_eth_kcp_rx_queue == NULL)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:audio_kcp_queue create error");
        goto error;
    }

    audio_ops.audio_kcp = ikcp_create(0x11223344,NULL);
    if(audio_ops.audio_kcp == NULL)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:audio_kcp create error");
        goto error;
    }

    
    kcp_pool_init();
    ikcp_setoutput(audio_ops.audio_kcp,kcp_output);
    ikcp_wndsize(audio_ops.audio_kcp,16,16);

    audio_ops.audio_kcp->rcv_wnd = 16;
    ikcp_nodelay(audio_ops.audio_kcp,1,10,2,1);
    ikcp_allocator(kcp_pool_malloc,kcp_pool_free);

    opus_pool_init();

    audio_ops.kcp_audio_mutex = xSemaphoreCreateMutex();
    if(audio_ops.kcp_audio_mutex == NULL)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"audio_init:kcp_audio_mutex create error");
        goto error;
    }

    touch_get_kcp((void **)&audio_ops.audio_kcp,audio_ops.kcp_audio_mutex);
    udp_get_kcp((void **)&audio_ops.audio_kcp,audio_ops.kcp_audio_mutex);

    err = udp_get_audio_rx_queue(audio_eth_rx_queue,audio_eth_kcp_rx_queue);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,"audio_init:audio_eth_rx_queue error");
        goto error;
    }
    
    err = audio_i2s_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"audio_i2s_init error: %s",esp_err_to_name(err));
        goto error;
    }

    err = audio_opus_init();
    if(err !=ESP_OK)
    {
        ESP_LOGE("opus","audio_opus_init error %s",esp_err_to_name(err));
        goto error;
    }



    if(xTaskCreatePinnedToCore(audio_kcp_rx_task,"audio_kcp_rx_task",(1024*2)+512,NULL,11,&audio_kcp_rx_task_handle,0) != pdPASS)
    {
        ESP_LOGE(TAG,"audio_init:audio_kcp_rx_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(audio_kcp_task,"audio_kcp_task",(1024*2)+512,NULL,10,&audio_kcp_task_handle,0) != pdPASS)
    {
        ESP_LOGE(TAG,"audio_init:audio_kcp_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(audio_rx_task,"audio_rx_task",(1024*2)+512,NULL,11,&audio_rx_task_handle,0) != pdPASS)
    {
        ESP_LOGE(TAG,"audio_init:audio_rx_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(opus_decode_task,"opus_decode_task",(1024*20)+512,NULL,12,&opus_decode_task_handle,1) != pdPASS)
    {
        ESP_LOGE(TAG,"video_init:jpeg_code_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(audio_task,"audio_task",(1024*5)+512,NULL,10,&audio_task_handle,1) != pdPASS)
    {
        ESP_LOGE(TAG,"audio_init:audio_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    ESP_LOGE(TAG,"初始化success");

    return ESP_OK;

error:

    if(audio_eth_rx_queue)
    {
        vQueueDelete(audio_eth_rx_queue);
        audio_eth_rx_queue = NULL;
    }

    if(audio_eth_kcp_rx_queue)
    {
        vQueueDelete(audio_eth_kcp_rx_queue);
        audio_eth_kcp_rx_queue = NULL;
    }
    
    for(uint8_t i=0;i<3;i++)
    {
        if(audio_ops.opus_recv_buf[i])
        {
            heap_caps_free(audio_ops.opus_recv_buf[i]);
            audio_ops.opus_recv_buf[i] = NULL;
        }
    }

    if(audio_ops.audio_kcp)
    {
        ikcp_release(audio_ops.audio_kcp);
        audio_ops.audio_kcp = NULL;
    }
        
    return err;
}



static void kcp_input_data(ikcpcb *kcp,struct netbuf *buf)
{
    if(buf != NULL)
    {
        struct pbuf *p = buf->p;

        while(p != NULL)
        {
            ikcp_input(kcp,(char*)p->payload,p->len);
            p = p->next;
        }
    }
}

void audio_kcp_rx_task(void *parm)
{
    (void)parm;

    wifi_group = (EventGroupHandle_t)get_wifi_eventgroup();
    if(wifi_group == NULL)
    {
        ESP_LOGE(TAG,"wifi_group get error");
        goto error;
    }
    struct netbuf *recv_buf = NULL;
    uint32_t      last_update = 0;

    ESP_LOGI(TAG,"started");

    while(1)
    {
        xEventGroupWaitBits(wifi_group,WIFI_CONNECTED_BIT,pdFALSE,pdTRUE,portMAX_DELAY);
        uint8_t notify = false;

        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        
        while(xQueueReceive(audio_eth_kcp_rx_queue,&recv_buf,pdMS_TO_TICKS(5)) == pdPASS)
        {
            if(audio_ops.audio_kcp && audio_ops.kcp_audio_mutex)
            {
                xSemaphoreTake(audio_ops.kcp_audio_mutex,portMAX_DELAY);
                kcp_input_data(audio_ops.audio_kcp,recv_buf);

                if(ikcp_peeksize(audio_ops.audio_kcp) > 0)
                {
                    notify = true;
                }
                xSemaphoreGive(audio_ops.kcp_audio_mutex);
            }

            if(recv_buf)
            {
                netbuf_delete(recv_buf);
                recv_buf = NULL;
            }

        }

        if ((uint32_t)(now_ms - last_update) >= 10)
        {
            last_update = now_ms;

            if (audio_ops.kcp_audio_mutex)
            {
                xSemaphoreTake(audio_ops.kcp_audio_mutex,portMAX_DELAY);
                if(audio_ops.audio_kcp)
                {
                    ikcp_update(audio_ops.audio_kcp,now_ms);
                    ikcp_flush(audio_ops.audio_kcp);
                }
                xSemaphoreGive(audio_ops.kcp_audio_mutex);
            }
        }

        if(notify && audio_kcp_task_handle != NULL)
        {
            notify = false;
            xTaskNotifyGive(audio_kcp_task_handle);
        }

        if(recv_buf)
        {
            netbuf_delete(recv_buf);
            recv_buf = NULL;
        }

    }

error:
    vTaskDelete(NULL);
    return;
}

void audio_kcp_info(void)
{
    if (audio_ops.audio_kcp == NULL) return;

    static ikcp_stat_t last;
    ikcp_stat_t st;

    ikcp_get_stat(audio_ops.audio_kcp, &st);

    ESP_LOGI(TAG,
             "KCP 链路 dup=%u(+%u) late=%u(+%u) gap=%u(+%u) | rcv_buf=%u rcv_que=%u rcv_wnd=%u rcv_nxt=%u rmt_wnd=%u | snd_buf=%u snd_que=%u | rto=%u srtt=%u state=%d",
             (unsigned)st.dup,  (unsigned)(st.dup  - last.dup),
             (unsigned)st.late, (unsigned)(st.late - last.late),
             (unsigned)st.gap,  (unsigned)(st.gap  - last.gap),
             (unsigned)st.nrcv_buf, (unsigned)st.nrcv_que,
             (unsigned)st.rcv_wnd,  (unsigned)st.rcv_nxt, (unsigned)st.rmt_wnd,
             (unsigned)st.nsnd_buf, (unsigned)st.nsnd_que,
             (unsigned)st.rto, (unsigned)st.srtt, (int)st.state);

    last = st;
}

void audio_kcp_task(void *parm)
{
    (void)parm;

    wifi_group = (EventGroupHandle_t)get_wifi_eventgroup();
    if(wifi_group == NULL)
    {
        ESP_LOGE(TAG,"wifi_group get error");
        goto error;
    }
    
    uint8_t *kcp_buf = heap_caps_malloc(2048,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(kcp_buf == NULL)
    {
        ESP_LOGE(TAG,"kcp_buf error");
        goto error;
    }

    memset(kcp_buf,0,2048);

    int recv_size = 0;

    while(1)
    {
        xEventGroupWaitBits(wifi_group,WIFI_CONNECTED_BIT,pdFALSE,pdTRUE,portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);

        xSemaphoreTake(audio_ops.kcp_audio_mutex,portMAX_DELAY);
        recv_size = ikcp_recv(audio_ops.audio_kcp,(char*)kcp_buf,2048);
        xSemaphoreGive(audio_ops.kcp_audio_mutex);

        if(recv_size <=0)
        {
            continue;
        }

        ESP_LOGI(TAG,"%s",kcp_buf);

    }

error:

    vTaskDelete(NULL);
    return;
}



void audio_rx_task(void *parm)
{
    (void)parm;
    
    wifi_group = (EventGroupHandle_t)get_wifi_eventgroup();
    if(wifi_group == NULL)
    {
        ESP_LOGE(TAG,"wifi_group get error");
        goto error;
    }


    struct netbuf *recv_buf = NULL;
    uint32_t ts         = 0;
    uint16_t seq        = 0;
    uint32_t ssrc       = 0;

    while(1)
    {
        xEventGroupWaitBits(wifi_group,WIFI_CONNECTED_BIT,pdFALSE,pdTRUE,portMAX_DELAY);

        while(xQueueReceive(audio_eth_rx_queue,&recv_buf,pdMS_TO_TICKS(5)) == pdPASS)
        {
            void *data_ptr;
            u16_t data_len;
            netbuf_data(recv_buf,&data_ptr,&data_len);

            if(data_len <= (int)sizeof(rtp_head_t))
            {
                ESP_LOGW(TAG,"bad rtp packet len=%d",data_len);
                if(recv_buf)
                {
                    netbuf_delete(recv_buf);
                    recv_buf = NULL;
                }
                continue;
            }

            uint8_t *payload = (uint8_t *)data_ptr;

            if(data_len >= 3 && payload[0] == 'F' && payload[1] == 'E' && payload[2] == 'C')
            {
                opus_fec_handle(payload,(uint16_t)data_len);
            }else
            {
                rtp_head_t *h       = (rtp_head_t *)payload;
                uint8_t     version = (uint8_t)(h->vpxcc >> 6);
                bool        marker  = (h->mpt & 0x80) != 0;
                uint8_t     pt      = (uint8_t)(h->mpt & 0x7F);
    
                if (version != RTP_VERSION || pt != RTP_PT_AUDIO)
                {
                    if(recv_buf)
                    {
                        netbuf_delete(recv_buf);
                        recv_buf = NULL;
                    }
                    continue;
                }
    
                ts         = ntohl(h->ts);
                seq        = ntohs(h->seq);
                ssrc       = ntohl(h->ssrc);
    
                size_t rtp_data_len = (size_t)(data_len - sizeof(rtp_head_t));
    
                if (rtp_data_len > 1024)
                {
                    ESP_LOGW(TAG,"bad rtp packet len=%d",rtp_data_len);
                    if(recv_buf)
                    {
                        netbuf_delete(recv_buf);
                        recv_buf = NULL;
                    }
                    continue;
                }

                if(marker)
                {
                    opus_queue_write(seq,(const uint8_t *)data_ptr,(uint32_t)data_len);
                }
                
            }

            if(recv_buf)
            {
                netbuf_delete(recv_buf);
                recv_buf = NULL;
            }

        }

        if(recv_buf)
        {
            netbuf_delete(recv_buf);
            recv_buf = NULL;
        }

    }

error:

    vTaskDelete(NULL);
    return;
}


size_t audio_queue_cnt = 0;
size_t low_water_cnt = 0;

void opus_decode_task(void *parm)
{
    (void)parm;
    uint8_t *out_buf  = heap_caps_malloc(2048,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t opus_out_bytes = 0;

    if(out_buf == NULL)
    {
        ESP_LOGE(TAG,"opus_decode_task buffer error");
        goto error;
    }

    while(1)
    {
        
        opus_pool_t *node = NULL;

        if(!opus_queue_pop(&node))
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if(node->flag == OPUS_FLAG_MISS)
        {
            uint32_t plc_bytes = 0;

            if(opus_plc_decode(out_buf,&plc_bytes) == ESP_OK && plc_bytes > 0)
            {
                audio_queue_push(&audio_ops,out_buf,plc_bytes/AUDIO_FRAME_BYTES,0);

            }else
            {
                ESP_LOGE(TAG,"opus_plc_decode error");
            }

            opus_queue_release(node);
            continue;
        }

        if((uint32_t)node->len > (uint32_t)sizeof(rtp_head_t))
        {
            uint32_t payload_len = (uint32_t)node->len - (uint32_t)sizeof(rtp_head_t);

            if(opus_to_pcm_decode(node->data + sizeof(rtp_head_t),payload_len,out_buf,2048,&opus_out_bytes) == ESP_OK)
            {
                if(audio_queue_push(&audio_ops,out_buf,opus_out_bytes/AUDIO_FRAME_BYTES,0) != true)
                {
                    ESP_LOGE(TAG,"audio_queue_push error");
                    audio_queue_cnt++;
                }
            }else
            {
                ESP_LOGE(TAG,"opus_to_pcm_decode error");
            }
        }

        opus_queue_release(node);

    }

error:
    vTaskDelete(NULL);
}

void audio_task(void *parm)
{
    (void)parm;

    static SemaphoreHandle_t audio_send_sema = NULL;
    const uint32_t frames_10ms = AUDIO_SAMPLE_RATE / 100;          /* 480 帧 */
    const uint32_t bytes_10ms  = frames_10ms * AUDIO_FRAME_BYTES;  /* 1920 字节 */

    const uint32_t prebuf_frames    = (AUDIO_SAMPLE_RATE * 120 / 1000);  /* 首启 120ms */
    const uint32_t low_water_frames = (AUDIO_SAMPLE_RATE * 20 / 1000);   /* 20ms 触发 underrun */
    const uint32_t resume_frames    = (AUDIO_SAMPLE_RATE * 40 / 1000);   /* 40ms 恢复 */

    bool started_once = false;
    bool underrun     = false;      /* 当前是否处于断流状态 */
    bool reported     = false;      /* 这次断流是否已经报过 */

    audio_send_sema = get_i2s_sema();

    while (1)
    {
        if(audio_send_sema)
        {
            xSemaphoreTake(audio_send_sema,portMAX_DELAY);
        }

        //portENTER_CRITICAL(&audio_ops.spinlock);
        uint32_t used = audio_queue_use(&audio_ops);
        //portEXIT_CRITICAL(&audio_ops.spinlock);

        uint32_t target = started_once ? resume_frames : prebuf_frames;

        if (used < target)
        {
            if (!underrun && started_once && used < low_water_frames && !reported)
            {
                ESP_LOGW(TAG,"underrun: used=%u frames, re-prebuffer",(unsigned)used);
                reported = true;
            }

            underrun = true;
            if(audio_send_sema)
                xSemaphoreGive(audio_send_sema);

            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        underrun = false;
        reported = false;
        started_once = true;

        audio_queue_read(&audio_ops,(uint16_t *)audio_ops.i2s_buf,frames_10ms);

        if(audio_queue_cnt != 0) audio_queue_cnt--;

        uint32_t done = 0;
        esp_err_t err = audio_i2s_write(audio_ops.i2s_buf,bytes_10ms,&done);

        if(err!= ESP_OK || done != bytes_10ms)
        {
            ESP_LOGW(TAG,"short write err=%d done=%u/%lu", err, (unsigned)done, (unsigned long)bytes_10ms);
        }
    }
}

