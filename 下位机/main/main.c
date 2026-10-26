#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "usb.h"
#include "led.h"
#include "wifi.h"
#include "lcd.h"
#include "jpeg.h"
#include "udp.h"
#include "video.h"
#include "audio.h"
#include "opus_pool.h"
#include "kcp_pool.h"
#include "opus_pool.h"
#include "touch.h"

static const char *TAG = "main";

void app_main(void)
{
    /* ---- USB 串口日志（TinyUSB CDC） ---- */
    ESP_ERROR_CHECK(usb_serial_init());

    /* 等主机打开串口再往下跑：否则主机还没开始读，CDC 的 TX FIFO 满了会丢日志 */
    const bool host = usb_serial_wait_connected(5000);
    ESP_LOGI(TAG, "console ready, host_connected=%d, reset_reason=%d",(int)host, (int)esp_reset_reason());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) 
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    led_init();
    
    ret = touch_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("touch","touch_init error %d",ret);
    }

    ret = lcd_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("LCD","lcd_init error %d",ret);
    }

    ret = jpeg_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("jpeg","jpeg_init error %d",ret);
    }

    wifi_init();

    ret = audio_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("audio","audio_init error %d",ret);
    }

    ret = video_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("video","video_init error %d",ret);
    }

    ret = eth_udp_init();
    if(ret !=ESP_OK)
    {
        ESP_LOGE("udp","eth_udp_init error %d",ret);
    }

    int8_t *buf = heap_caps_malloc(2048,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(buf == NULL)
    {
        ESP_LOGE("led_task","heap_caps_malloc error\r\n");
        return;
    }

    size_t cnt = 0;

    while (1)
    {
        led_toggle();
        if(cnt%5 == 0)
        {
            show_video_info();
            kcp_pool_info();
            ops_pool_info();

            memset(buf,0,2048);
            vTaskGetRunTimeStats((char*)buf);
            ESP_LOGI("led_task","\r\n%s\r\n",buf);
    
            size_t free_iram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    
            // 2. 获取内部 SRAM 中【最大的一块连续空闲内存】（这决定了你一次性最多能 malloc 多大的数组）
            size_t max_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    
            // 3. 获取历史最低可用内存（高水位线，用于评估系统极端情况下的内存压力）
            size_t min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    
            ESP_LOGI("MEM", "--- 内部 SRAM 状态 ---");
            ESP_LOGI("MEM", "总剩余内存: %d 字节 (%.1f KB)", free_iram,free_iram / 1024.0);
            ESP_LOGI("MEM", "最大连续块: %d 字节 (%.1f KB)", max_block,max_block / 1024.0);
            ESP_LOGI("MEM", "历史最低值: %d 字节 (%.1f KB)", min_free,min_free / 1024.0);
            ESP_LOGI("MEM", "------------------------");

    
            
            // 1. 获取 PSRAM 总剩余内存
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    
            // 2. 获取 PSRAM 中【最大的一块连续空闲内存】
            size_t max_block_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    
            // 3. 获取 PSRAM 历史最低可用内存
            size_t min_free_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    
            ESP_LOGI("MEM", "--- 外部 PSRAM 状态 ---");
            ESP_LOGI("MEM", "总剩余内存: %u 字节 (%.2f MB)", free_psram, free_psram / (1024.0 * 1024.0));
            ESP_LOGI("MEM", "最大连续块: %u 字节 (%.2f MB)", max_block_psram, max_block_psram / (1024.0 * 1024.0));
            ESP_LOGI("MEM", "历史最低值: %u 字节 (%.2f MB)", min_free_psram, min_free_psram / (1024.0 * 1024.0));
            ESP_LOGI("MEM", "------------------------");
        }
        //audio_kcp_info();       /* 每秒一行：dup/late 有没有在涨 */
        //ops_pool_info();        /* 每秒一行：写/读/丢 + 池占用 */
        cnt++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

