#include "lcd.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "jpeg.h"

static const char *TAG = "lcd";

void lcd_switch_task(void *parm);
TaskHandle_t lcd_switch_task_handle = NULL;


uint16_t *lcd_frame = NULL;
/* ======================= 控制信号引脚 ======================= */
#define LCD_PIN_HSYNC       GPIO_NUM_44
#define LCD_PIN_VSYNC       GPIO_NUM_45
#define LCD_PIN_DE          GPIO_NUM_43
#define LCD_PIN_PCLK        GPIO_NUM_40

/* ======================= 数据线顺序（24 位 RGB888） =======================
 * 驱动要求：数组下标 0..7 = 蓝 LSB..MSB，8..15 = 绿，16..23 = 红。
 * 逐位对应硬件引脚表：
 *   R0-R7 = IO8  IO9  IO10 IO11 IO12 IO13 IO14 IO15
 *   G0-G3 = IO16 IO17 IO18 IO19 / G4-G7 = IO33 IO34 IO35 IO36
 *   B0-B2 = IO37 IO38 IO39 / B3-B5 = IO2 IO3 IO4 / B6 = IO5 / B7 = IO7
 */
static const gpio_num_t lcd_data_gpios[24] = {
    // ---- 蓝 B0..B7 ----
    GPIO_NUM_37,  // [0]  B0
    GPIO_NUM_38,  // [1]  B1
    GPIO_NUM_39,  // [2]  B2
    GPIO_NUM_2,   // [3]  B3
    GPIO_NUM_3,   // [4]  B4
    GPIO_NUM_4,   // [5]  B5
    GPIO_NUM_5,   // [6]  B6
    GPIO_NUM_7,   // [7]  B7
    // ---- 绿 G0..G7 ----
    GPIO_NUM_16,  // [8]  G0
    GPIO_NUM_17,  // [9]  G1
    GPIO_NUM_18,  // [10] G2
    GPIO_NUM_19,  // [11] G3
    GPIO_NUM_33,  // [12] G4
    GPIO_NUM_34,  // [13] G5
    GPIO_NUM_35,  // [14] G6
    GPIO_NUM_36,  // [15] G7
    // ---- 红 R0..R7 ----
    GPIO_NUM_8,   // [16] R0
    GPIO_NUM_9,   // [17] R1
    GPIO_NUM_10,  // [18] R2
    GPIO_NUM_11,  // [19] R3
    GPIO_NUM_12,  // [20] R4
    GPIO_NUM_13,  // [21] R5
    GPIO_NUM_14,  // [22] R6
    GPIO_NUM_15,  // [23] R7
};


static esp_lcd_panel_handle_t lcd_panel_handle;

/* ======================= 面板参数 =======================*/

lcd_info_t lcd = {
    .height  = 480,
    .width   = 800,
    .pixsize = 24,
    .clk     = 28 * 1000000,

    .hspw = 48,
    .hbp  = 88,
    .hfp  = 40,

    .vspw = 3,
    .vbp  = 32,
    .vfp  = 13,

    .timing_flag.hsync_idle_low  = true,
    .timing_flag.vsync_idle_low  = true,
    .timing_flag.de_idle_high    = false,
    .timing_flag.pclk_idle_high  = true,
    .timing_flag.pclk_active_neg = true,

    .lcd_buf.spinlock = portMUX_INITIALIZER_UNLOCKED,
    .lcd_buf.new_frame = false,
    .lcd_buf.jpeg_queue = NULL,
    .lcd_buf_num = LCD_NUM_FBS,
    .lcd_buf.lcd_buf[0] = NULL,
    .lcd_buf.lcd_buf[1] = NULL,
    .lcd_buf.lcd_buf[2] = NULL,
};


static bool IRAM_ATTR lcd_frame_on_vsync_cb(esp_lcd_panel_handle_t panel,const esp_lcd_rgb_panel_event_data_t *edata,void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    BaseType_t high_task_awoken = pdFALSE;

    if(lcd_switch_task_handle)
    {
        vTaskNotifyGiveFromISR(lcd_switch_task_handle,&high_task_awoken);
    }

    return high_task_awoken == pdTRUE;
}

static bool IRAM_ATTR lcd_frame_done_cb(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *edata,void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    BaseType_t hp_task_woken = pdFALSE;

    if(lcd_frame)
    {
        (*lcd_frame)++;
    }


    return hp_task_woken == pdTRUE;
}

/* ======================= 初始化 ======================= */
esp_err_t lcd_init(void)
{
    esp_err_t err = ESP_FAIL;

    lcd.lcd_buf.jpeg_queue = xQueueCreate(1,sizeof(jpeg_data_t));
    if(lcd.lcd_buf.jpeg_queue == NULL) 
    {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "lcd.lcd_buf.jpeg_queue failed: %s", esp_err_to_name(err));
        goto error;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_PIN_RES),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&io_conf);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        goto error;
    }

    lcd_reset_panel();
    lcd_backlight(true);

    
    for (int i = 0; i < LCD_NUM_FBS; i++) 
    {
        lcd.lcd_buf.lcd_buf[i] = heap_caps_aligned_calloc(128, 1, (size_t)lcd.width * lcd.height * (lcd.pixsize / 8),
                                                  MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
                                                  
        if (lcd.lcd_buf.lcd_buf[i] == NULL) 
        {
            ESP_LOGE(TAG, "帧缓冲 %d 分配失败", i);
            err = ESP_ERR_NO_MEM;
            goto error;
        }
    }

    lcd.lcd_buf.front = lcd.lcd_buf.lcd_buf[0];
    lcd.lcd_buf.ready = lcd.lcd_buf.lcd_buf[1];
    lcd.lcd_buf.back = lcd.lcd_buf.lcd_buf[2];

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src          = LCD_CLK_SRC_DEFAULT,
        .data_width       = lcd.pixsize,
        .in_color_format  = LCD_COLOR_FMT_RGB888,
        .out_color_format = LCD_COLOR_FMT_RGB888,
        .num_fbs          = lcd.lcd_buf_num,
        .bounce_buffer_size_px = 0,
        .dma_burst_size   = 64,
        .hsync_gpio_num   = LCD_PIN_HSYNC,
        .vsync_gpio_num   = LCD_PIN_VSYNC,
        .de_gpio_num      = LCD_PIN_DE,
        .pclk_gpio_num    = LCD_PIN_PCLK,
        .disp_gpio_num    = -1,

        .timings = {
            .pclk_hz            = lcd.clk,
            .h_res              = lcd.width,
            .v_res              = lcd.height,
            .hsync_pulse_width  = lcd.hspw,
            .hsync_back_porch   = lcd.hbp,
            .hsync_front_porch  = lcd.hfp,
            .vsync_pulse_width  = lcd.vspw,
            .vsync_back_porch   = lcd.vbp,
            .vsync_front_porch  = lcd.vfp,
            .flags = {
                .hsync_idle_low  = lcd.timing_flag.hsync_idle_low,
                .vsync_idle_low  = lcd.timing_flag.vsync_idle_low,
                .de_idle_high    = lcd.timing_flag.de_idle_high,
                .pclk_active_neg = lcd.timing_flag.pclk_active_neg,
                .pclk_idle_high  = lcd.timing_flag.pclk_idle_high,
            },
        },

        .flags = {
            .disp_active_low     = 0,
            .refresh_on_demand   = 0,   /* 持续扫描输出 */
            .fb_in_psram         = 1,
            .no_fb               = 0,
            .bb_invalidate_cache = 0,
        },

        .user_fbs[0] = lcd.lcd_buf.lcd_buf[0],
        .user_fbs[1] = lcd.lcd_buf.lcd_buf[1],
        .user_fbs[2] = lcd.lcd_buf.lcd_buf[2],
    };
    memcpy(cfg.data_gpio_nums, lcd_data_gpios, sizeof(lcd_data_gpios));

    err = esp_lcd_new_rgb_panel(&cfg, &lcd_panel_handle);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG,"esp_lcd_new_rgb_panel failed");
        goto error;
    }


    esp_lcd_rgb_panel_event_callbacks_t cbs = 
    {
        .on_vsync = lcd_frame_on_vsync_cb,
        .on_frame_buf_complete = lcd_frame_done_cb,
    };

    err = esp_lcd_rgb_panel_register_event_callbacks(lcd_panel_handle,&cbs,NULL);
    
    if(err != ESP_OK) 
    {
        ESP_LOGE(TAG, "register callbacks failed: %s",esp_err_to_name(err));
        goto error;
    }

    err = esp_lcd_panel_reset(lcd_panel_handle);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "panel reset failed: %s", esp_err_to_name(err));
        goto error;
    }
    err = esp_lcd_panel_init(lcd_panel_handle);
    if (err != ESP_OK) 
    {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        goto error;
    }

    //lcd_show_color(LCD_COLOR_YELLOW);

    if(xTaskCreatePinnedToCore(lcd_switch_task,"lcd_switch_task",(1024*2)+512,NULL,configMAX_PRIORITIES - 1,&lcd_switch_task_handle,1) != pdPASS)
    {
        ESP_LOGE(TAG,"audio_init:audio_task create error");

        err = ESP_ERR_NO_MEM;
        goto error;
    }

    return ESP_OK;

error:
    for (int i = 0; i < lcd.lcd_buf_num; i++) 
    {
        if(lcd.lcd_buf.lcd_buf[i]) 
        {
            heap_caps_free(lcd.lcd_buf.lcd_buf[i]);
            lcd.lcd_buf.lcd_buf[i] = NULL;
        }
    }

    if(lcd.lcd_buf.jpeg_queue)
    {
        vQueueDelete(lcd.lcd_buf.jpeg_queue);
    }

    if(lcd_panel_handle) 
    {
        esp_lcd_panel_del(lcd_panel_handle);
        lcd_panel_handle = NULL;
    }
    return err;
}


/* ======================= 背光 / 复位 ======================= */
void lcd_reset_panel(void)
{
    gpio_set_level(LCD_PIN_RES, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_PIN_RES, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void lcd_backlight(bool on)
{
    gpio_set_level(LCD_PIN_BL,on);
}

void lcd_get_frame(uint16_t *frame)
{
    if(frame != NULL)
    {
        lcd_frame = frame;
    }
}

void lcd_switch_task(void *parm)
{
    while(1)
    {
        uint8_t *front = NULL;
        
        ulTaskNotifyTake(pdTRUE,portMAX_DELAY);

        if(!lcd.lcd_buf.new_frame) continue;

        portENTER_CRITICAL(&lcd.lcd_buf.spinlock);
        if(lcd.lcd_buf.new_frame)
        {
            lcd.lcd_buf.front = lcd.lcd_buf.ready;
            front = lcd.lcd_buf.front;
            lcd.lcd_buf.new_frame = false;
        }
        portEXIT_CRITICAL(&lcd.lcd_buf.spinlock);

        if(front)
            esp_lcd_panel_draw_bitmap(lcd_panel_handle,0,0,lcd.width,lcd.height,front);
    }
}
