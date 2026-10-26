#include "jpeg.h"
#include "driver/jpeg_decode.h"
#include <inttypes.h>
#include <string.h>
#include "lcd.h"
#include "esp_log.h"

static const char *TAG = "jpeg";
static jpeg_decoder_handle_t decoder_handle  = NULL;

static uint32_t              fb_bytes = 0;   /* 帧缓冲字节数 = w*h*3 */

esp_err_t jpeg_init(void)
{
    esp_err_t err = ESP_FAIL;

    if (decoder_handle != NULL)
    {
        return ESP_OK;
    }

    fb_bytes = (uint32_t)lcd.width * lcd.height * (lcd.pixsize / 8);
    
    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,     /* 0 = 驱动自选 */
        .timeout_ms    = 60,   /* 单帧超时：30fps 需要 > 34ms */
    };
    err = jpeg_new_decoder_engine(&eng_cfg, &decoder_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG,"jpeg_new_decoder_engine failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "decoder create success");
    return ESP_OK;
}

esp_err_t jpeg_decode_show(uint8_t *pdata_buf,uint32_t jpeg_size)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if(!pdata_buf)
    {
        return err;
    }

    if ((uintptr_t)pdata_buf & 0x03)
    {
        ESP_LOGE(TAG, "pdata_buf 非 4 字节对齐: %p", pdata_buf);
        return err;
    }

    /* ---- 解析头部拿尺寸 ---- */
    // jpeg_decode_picture_info_t info;
    // err = jpeg_decoder_get_info(pdata_buf,jpeg_size,&info);
    // if (err != ESP_OK)
    // {
    //     ESP_LOGE(TAG,"jpeg_decoder_get_info failed: %s", esp_err_to_name(err));
    //     return err;
    // }


    // if (info.height % 16 != 0 || info.width % 16 != 0) 
    // {
    //     ESP_LOGE(TAG, "图片尺寸不是16的倍数,无法零拷贝!");
    //     return ESP_ERR_INVALID_SIZE;
    // }

    // if ((uint32_t)fb_bytes != info.height * info.width * (lcd.pixsize / 8)) 
    // {
    //     ESP_LOGE(TAG, "framebuffer size mismatch");
    //     return ESP_ERR_INVALID_SIZE;
    // }

    portENTER_CRITICAL(&lcd.lcd_buf.spinlock);
    uint8_t *fb = lcd.lcd_buf.back;
    portEXIT_CRITICAL(&lcd.lcd_buf.spinlock);
    if (fb == NULL) 
    {
        ESP_LOGE(TAG, "framebuffer is NULL");
        return err;
    }

    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    uint32_t out_size = 0;
    err = jpeg_decoder_process(decoder_handle,&cfg,
                                        pdata_buf, jpeg_size,
                                        fb, (uint32_t)fb_bytes,
                                        &out_size);

    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "jpeg_decoder_process failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}
