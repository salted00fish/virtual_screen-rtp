#include "audio_i2s.h"
#include "audio_config.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "AUDIO_I2S"

static i2s_chan_handle_t i2s_rx_handle      = NULL;
static SemaphoreHandle_t i2s_send_sema      = NULL;  

static bool IRAM_ATTR on_i2s_sent(i2s_chan_handle_t handle,i2s_event_data_t *event, void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;

    if(i2s_send_sema)
        xSemaphoreGiveFromISR(i2s_send_sema,&hp_woken);

    return hp_woken == pdTRUE;
}

static esp_err_t sd_pin_config(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << AUDIO_PIN_SD),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "SD_MODE(IO%d) 配置失败", (int)AUDIO_PIN_SD);
    return gpio_set_level(AUDIO_PIN_SD,1);
}




esp_err_t audio_i2s_init(void)
{
    sd_pin_config();
    vTaskDelay(pdMS_TO_TICKS(10));

    i2s_send_sema = xSemaphoreCreateCounting(AUDIO_DMA_DESC_NUM - 1,AUDIO_DMA_DESC_NUM - 1);
    if(i2s_send_sema == NULL)
    {
        ESP_LOGE(TAG,"i2s_send_sema");
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = {
        .id            = I2S_NUM_AUTO,
        .role          = I2S_ROLE_MASTER,
        .dma_desc_num  = AUDIO_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_DMA_FRAME_NUM,
        .auto_clear    = true,
    };

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg,&i2s_rx_handle,NULL),TAG,"申请 I2S 发送通道失败");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_PIN_BCLK,
            .ws   = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    esp_err_t err = i2s_channel_init_std_mode(i2s_rx_handle,&std_cfg);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG,"初始化标准模式失败: %s",esp_err_to_name(err));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = NULL;
        return err;
    }

    i2s_event_callbacks_t cbs = {
        .on_sent        = on_i2s_sent,
    };

    i2s_channel_register_event_callback(i2s_rx_handle,&cbs, NULL);

    err = i2s_channel_enable(i2s_rx_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"使能 I2S 通道失败: %s",esp_err_to_name(err));
        i2s_del_channel(i2s_rx_handle);
        i2s_rx_handle = NULL;
        return err;
    }
    
    ESP_LOGE(TAG,"初始化success");
    return ESP_OK;
}

esp_err_t audio_i2s_write(uint8_t *pcm,uint32_t size,uint32_t *write_byte)
{
    return i2s_channel_write(i2s_rx_handle,pcm,size,(size_t *)write_byte,pdMS_TO_TICKS(100));   /* 单位是 ms，不是 tick */
}



void *get_i2s_sema(void)
{
    return i2s_send_sema;
}
