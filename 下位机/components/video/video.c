#include "video.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"

#include "wifi.h"
#include "udp.h"
#include "esp_log.h"
#include "lwip/api.h"
#include "jpeg.h"
#include "lcd.h"

#define TAG "video"

static EventGroupHandle_t wifi_group  = NULL;
static QueueHandle_t      video_eth_rx_queue = NULL;

#define RTP_MAX_FRAME_SIZE          (256 * 1024)
#define RTP_FRAME_SLOTS             3

typedef enum
{
    RX_IDLE = 0,    /* 等一帧的第一个分片 */
    RX_FILL,        /* 正在往帧缓冲里拼 */
} video_rx_state_t;

typedef struct 
{
    uint8_t *video_recv_buf[RTP_FRAME_SLOTS];
    uint8_t *front;
    uint8_t *ready;
    uint8_t *back;

    uint32_t write_index;
    size_t  ready_len;
    uint8_t ready_used;

    portMUX_TYPE spinlock;
    rtp_states_t rtp_states;
    video_rx_state_t rx_state;
}video_t;

video_t video = {};

void video_rx_task(void *parm);
TaskHandle_t video_rx_task_handle = NULL;

void jpeg_code_task(void *parm);
TaskHandle_t jpeg_code_task_handle = NULL;

esp_err_t video_init(void)
{
    esp_err_t err = ESP_OK;
    lcd_get_frame(&video.rtp_states.fps);
    video_eth_rx_queue = xQueueCreate(32, sizeof(int));
    if(video_eth_rx_queue == NULL)
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,"video_init:video_queue create error");
        goto error;
    }
    
    err = udp_get_vidio_rx_queue(video_eth_rx_queue);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,"video_init:udp_get_vidio_rx_queue error");
        goto error;
    }

    for(uint8_t i=0;i<RTP_FRAME_SLOTS;i++)
    {
        video.video_recv_buf[i] = heap_caps_aligned_calloc(128,1,RTP_MAX_FRAME_SIZE,MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

        if(video.video_recv_buf[i] == NULL)
        {
            ESP_LOGE(TAG,"video_recv_buf %d error",i);
            goto error;
        }
    }

    spinlock_initialize(&video.spinlock);

    video.front =video.video_recv_buf[0];
    video.ready =video.video_recv_buf[1];
    video.back  =video.video_recv_buf[2];

    video.rx_state = RX_IDLE;
    video.write_index = 0;

    if(xTaskCreatePinnedToCore(video_rx_task,"video_rx_task",(1024*2)+512,NULL,11,&video_rx_task_handle,1) != pdPASS)
    {
        ESP_LOGE(TAG,"video_init:video_rx_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    if(xTaskCreatePinnedToCore(jpeg_code_task,"jpeg_code_task",(1024*2)+512,NULL,10,&jpeg_code_task_handle,1) != pdPASS)
    {
        ESP_LOGE(TAG,"video_init:jpeg_code_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    return ESP_OK;

error:

    if(video_eth_rx_queue)
    {
        vQueueDelete(video_eth_rx_queue);
        video_eth_rx_queue = NULL;
    }

    for(uint8_t i=0;i<RTP_FRAME_SLOTS;i++)
    {
        if(video.video_recv_buf[i])
        {
            heap_caps_free(video.video_recv_buf[i]);
            video.video_recv_buf[i] = NULL;
        }
    }

    if(video_rx_task_handle)
    {
        vTaskDelete(video_rx_task_handle);
    }

    ESP_LOGE(TAG,"success");
    return err;
}

void switch_buf(void)
{
    for(uint8_t i=0;i<RTP_FRAME_SLOTS;i++)
    {
        if(video.video_recv_buf[i] != video.front && video.video_recv_buf[i] != video.ready && video.video_recv_buf[i] != video.back)
        {
            video.back = video.video_recv_buf[i];
            break;
        }
    }
}


void video_rx_task(void *parm)
{
    wifi_group = (EventGroupHandle_t)get_wifi_eventgroup();
    if(wifi_group == NULL)
    {
        ESP_LOGE(TAG,"wifi_group get error");
        goto error;
    }

    struct netbuf *recv_buf = NULL;
    void *data = NULL;
    uint32_t current_ts = 0;
    uint16_t expect_seq = 0;
    uint8_t drop_data = false;

    while(1)
    {
        xEventGroupWaitBits(wifi_group,WIFI_CONNECTED_BIT,pdFALSE,pdTRUE,portMAX_DELAY);
        xQueueReceive(video_eth_rx_queue,&recv_buf,portMAX_DELAY);

        uint16_t recv_len = 0;
        uint16_t data_len = 0;

        recv_len = netbuf_len(recv_buf);
        netbuf_data(recv_buf,&data,&data_len);

        if(recv_len < sizeof(rtp_head_t) || data_len < sizeof(rtp_head_t))
        {
            video.rtp_states.bad_packets++;
            netbuf_delete(recv_buf);
            continue;
        }

        rtp_head_t *h       = (rtp_head_t *)data;
        uint8_t     version = (uint8_t)(h->vpxcc >> 6);
        bool        marker  = (h->mpt & 0x80) != 0;
        uint8_t     pt      = (uint8_t)(h->mpt & 0x7F);

        if (version != RTP_VERSION || pt != RTP_PT_JPEG)
        {
            video.rtp_states.bad_packets++;
            netbuf_delete(recv_buf);
            continue;
        }

        uint32_t ts         = ntohl(h->ts);
        uint16_t seq        = ntohs(h->seq);
        uint32_t ssrc       = ntohl(h->ssrc);

        size_t   rtp_data_len = (size_t)(data_len - sizeof(rtp_head_t));
        uint8_t *rtp_data = ((uint8_t*)data) + sizeof(rtp_head_t);

        video.rtp_states.packets++;
        video.rtp_states.ssrc   = ssrc;

        if(video.rx_state == RX_IDLE || current_ts != ts)
        {
            if(video.rx_state == RX_FILL)
            {
                /**
                 * 时间戳对不上，出现新帧了
                 */
                video.rtp_states.dropped++;
            }


            current_ts = ts;
            expect_seq = seq;
            drop_data = false;

            video.write_index = 0;
            video.rx_state = RX_FILL;
        }

        if(video.rx_state == RX_FILL)
        {
            if (video.write_index == 0 && !drop_data) 
            {
                if (rtp_data_len < 2 || rtp_data[0] != 0xFF || rtp_data[1] != 0xD8) 
                {
                    drop_data = true;
                }
            }

            if(!drop_data && expect_seq == seq && (video.write_index + rtp_data_len) <= RTP_MAX_FRAME_SIZE)
            {
                netbuf_copy_partial(recv_buf,video.back + video.write_index,rtp_data_len,sizeof(rtp_head_t));
                video.write_index += rtp_data_len;
            }else
            {
                /**
                 * 期望序列号与接收的序列号对不上，后面数据丢弃，直到新的一帧到来
                 */
                drop_data = true;
            }

            expect_seq++;

            if(marker)
            {
                if(drop_data || video.write_index == 0)
                {
                    video.rtp_states.dropped++;

                }else
                {
                    portENTER_CRITICAL(&video.spinlock);

                    if (video.ready_used)
                    {
                        video.rtp_states.dropped++; 
                    }

                    video.ready = video.back;
                    video.ready_len = video.write_index;
                    video.ready_used = true;

                    switch_buf();
                    portEXIT_CRITICAL(&video.spinlock);

                    if(jpeg_code_task_handle)
                    {
                        uint32_t current_notify_value = 0;

                        xTaskNotifyAndQuery(jpeg_code_task_handle,0,eNoAction,&current_notify_value);

                        if(current_notify_value <= 2)
                        {
                            xTaskNotifyGive(jpeg_code_task_handle);
                        }
                    }
                        
                }

                video.rx_state = RX_IDLE;
            }
        }

        netbuf_delete(recv_buf);
    }

error:
    vTaskDelete(NULL);
    return;
}

void jpeg_code_task(void *parm)
{
    (void)parm;

    while(1)
    {
        ulTaskNotifyTake(pdFALSE,portMAX_DELAY);
        uint8_t *current_buf = NULL;
        uint32_t current_len = 0;

        portENTER_CRITICAL(&video.spinlock);

        if (video.ready_used) 
        {
            video.front = video.ready;
            video.ready_used = false; // 标记已被取走

            current_len = video.ready_len;
            current_buf = video.front;
        }
        portEXIT_CRITICAL(&video.spinlock);

        if(current_buf == NULL || current_len == 0)
        {
            continue;
        }

        int64_t  t0       = esp_timer_get_time();
        esp_err_t show_err = jpeg_decode_show(current_buf,current_len);

        if (show_err != ESP_OK)
        {
            ESP_LOGE("jpeg_code_task","jpeg_decode_show failed: %s, frame dropped",esp_err_to_name(show_err));
            continue;
        }
        
        video.rtp_states.last_decode_us = (uint32_t)(esp_timer_get_time() - t0);
        video.rtp_states.frames++;

        portENTER_CRITICAL(&lcd.lcd_buf.spinlock);
        lcd.lcd_buf.ready = lcd.lcd_buf.back;
        lcd.lcd_buf.new_frame = true;

        for (int i = 0; i < lcd.lcd_buf_num; i++) 
        {
            if (lcd.lcd_buf.lcd_buf[i] != lcd.lcd_buf.front && lcd.lcd_buf.lcd_buf[i] != lcd.lcd_buf.ready)
            {
                lcd.lcd_buf.back = lcd.lcd_buf.lcd_buf[i];
                break;
            }
        }
        portEXIT_CRITICAL(&lcd.lcd_buf.spinlock);
    }
}

void show_video_info(void)
{
    ESP_LOGW(TAG,"接收%lu帧  丢弃%lu帧 帧率：%lu 解码:%u us",video.rtp_states.frames,video.rtp_states.dropped,
                                                                        video.rtp_states.fps/5,video.rtp_states.last_decode_us);
    video.rtp_states.fps = 0;
}