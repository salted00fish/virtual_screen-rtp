#include "audio_queue.h"
#include "audio_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"


#define TAG "audio_queue"

esp_err_t audio_queue_init(uint8_t **audio_queue,uint8_t **i2s_buf)
{
    esp_err_t err = ESP_OK;
    *audio_queue = heap_caps_aligned_calloc(64,AUDIO_RING_FRAMES * AUDIO_CHANNELS,sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(!(*audio_queue))
    {
        ESP_LOGE(TAG,"audio_queue malloc error");
        err = ESP_ERR_NO_MEM;
        goto error;
    }

    *i2s_buf = heap_caps_aligned_calloc(64,(AUDIO_SAMPLE_RATE / 100) + AUDIO_ADJ_FRAMES,AUDIO_FRAME_BYTES,MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if(!(*i2s_buf))
    {
        ESP_LOGE(TAG,"i2s_buf malloc error");
        err = ESP_ERR_NO_MEM;
        goto error;
    }
    

    ESP_LOGE(TAG,"success\r\n");
    return ESP_OK;

error:
    
    if(*audio_queue)
    {
        heap_caps_free(*audio_queue);
        *audio_queue = NULL;
    }

    if(*i2s_buf)
    {
        heap_caps_free(*i2s_buf);
        *i2s_buf = NULL;
    }

    return err;
}

uint32_t audio_queue_use(audio_ops_t *ops)
{

    uint32_t use = ops->write + AUDIO_RING_FRAMES - ops->read;

    if(use >= AUDIO_RING_FRAMES)
    {
        use -= AUDIO_RING_FRAMES;
    }

    return use;
}

static uint32_t audio_queue_drop(audio_ops_t *ops,uint32_t frame)
{

    portENTER_CRITICAL(&ops->spinlock);
    uint32_t use = audio_queue_use(ops);
    if(frame > use)
    {
        frame = use;
    }

    ops->read +=frame;
    if(ops->read >= AUDIO_RING_FRAMES)
    {
        ops->read -= AUDIO_RING_FRAMES;
    }

    portEXIT_CRITICAL(&ops->spinlock);

    return frame;
}


static uint32_t audio_queue_write(audio_ops_t *ops,uint8_t *pcm,uint32_t frame)
{
    uint32_t write_index = 0;

    portENTER_CRITICAL(&ops->spinlock);
    uint32_t used = audio_queue_use(ops);
    uint32_t free_frame = AUDIO_RING_FRAMES - used - 1;
    uint32_t write_frame = (frame > free_frame) ? free_frame:frame;

    if(write_frame > 0)
    {
        write_index = ops->write;
    }
    portEXIT_CRITICAL(&ops->spinlock);

    if(write_frame > 0)
    {
        uint32_t first = AUDIO_RING_FRAMES - write_index;
        
        /**
         * 处理数据回绕情况
         * 
         */
        if(first > write_frame)
        {
            /**
             * 要写入的数据大小 小于 音频队列剩余的大小
             */
            first = write_frame;
        }

        memcpy(ops->audio_queue + (write_index*AUDIO_FRAME_BYTES),pcm,first*AUDIO_FRAME_BYTES);

        if(first < write_frame)
        {
            memcpy(ops->audio_queue,pcm + (first*AUDIO_FRAME_BYTES),(write_frame - first)*AUDIO_FRAME_BYTES);
        }

        portENTER_CRITICAL(&ops->spinlock);

        ops->write += write_frame;
        if(ops->write >= AUDIO_RING_FRAMES)
        {
            ops->write -= AUDIO_RING_FRAMES;
        }

        portEXIT_CRITICAL(&ops->spinlock);
    }

    return write_frame;
}

uint32_t audio_queue_read(audio_ops_t *ops,uint16_t *dst, uint32_t want)
{
    uint32_t read_frame = 0;
    uint32_t read_index = 0;

    portENTER_CRITICAL(&ops->spinlock);
    uint32_t use = audio_queue_use(ops);
    read_frame = (want > use) ? use:want;

    if(read_frame > 0)
    {
        read_index = ops->read;
    }

    portEXIT_CRITICAL(&ops->spinlock);

    if(read_frame > 0)
    {
        uint32_t first = AUDIO_RING_FRAMES - read_index;

        if(first > read_frame)
        {
            first = read_frame;
        }

        memcpy(dst,ops->audio_queue+(read_index*AUDIO_FRAME_BYTES),first*AUDIO_FRAME_BYTES);

        if(first < read_frame)
        {
            memcpy((char *)dst+(first*AUDIO_FRAME_BYTES),ops->audio_queue,(read_frame - first)*AUDIO_FRAME_BYTES);
        }

        portENTER_CRITICAL(&ops->spinlock);

        ops->read +=read_frame;

        if(ops->read >= AUDIO_RING_FRAMES)
        {
            ops->read -= AUDIO_RING_FRAMES;
        }

        portEXIT_CRITICAL(&ops->spinlock);
    }

    return read_frame;
}

bool audio_queue_push(audio_ops_t *ops,uint8_t *pcm,uint32_t frame,uint32_t pts_ms)
{
    if (pcm == NULL || frame == 0 || ops == NULL)
    {
        return false;
    }

    uint32_t write_count = 0;
    write_count = audio_queue_write(ops,pcm,frame);
    if(write_count < frame)
    {
        uint32_t need_drop = frame - write_count;
        audio_queue_drop(ops,need_drop);

        write_count += audio_queue_write(ops,pcm+write_count*AUDIO_FRAME_BYTES,need_drop);
    }

    return frame == write_count;
}


void audio_switch_buf(audio_ops_t *ops)
{
    for(uint8_t i=0;i<3;i++)
    {
        if(ops->opus_recv_buf[i] != ops->opus_recv_front && ops->opus_recv_buf[i] != ops->opus_recv_ready && ops->opus_recv_buf[i] != ops->opus_recv_back)
        {
            ops->opus_recv_back = ops->opus_recv_buf[i];
            break;
        }
    }
}
