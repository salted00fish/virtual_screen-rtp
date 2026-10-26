#ifndef LCD_H
#define LCD_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#include "board_lcd_config.h"
#include "esp_err.h"
#include "esp_lcd_io_i80.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"

/*
 * 正点原子 ATK-MD0430R（4.3 寸 800x480 RGB888 屏）
 *
 * 方案：PSRAM 双缓冲 + RGB DMA 持续扫描输出
 *   每块帧缓冲 800*480*3 = 1,152,000 字节（1.10 MiB），两块共 2.25 MiB。
 *   CPU 往"后台缓冲"画，画完调 lcd_flush() 切换，面板 DMA 从"前台缓冲"读。
 *
 * 引脚：
 *   背光   LCD_BL  = SD_CLK = GPIO24（硬件短接，见 board_lcd_config.h）
 *   复位   LCD_RES = SD_D0  = GPIO20，低电平有效
 *   PCLK=GPIO40  DE=GPIO43  HSYNC=GPIO44  VSYNC=GPIO45
 *
 *   数据线（24 位，驱动要求顺序 蓝0-7, 绿0-7, 红0-7）：
 *     蓝 B0-B7 : IO37 IO38 IO39 IO2  IO3  IO4  IO5  IO7
 *     绿 G0-G7 : IO16 IO17 IO18 IO19 IO33 IO34 IO35 IO36
 *     红 R0-R7 : IO8  IO9  IO10 IO11 IO12 IO13 IO14 IO15
 */

#define LCD_H_RES               800
#define LCD_V_RES               480


/* ---- 背光 / 复位 ----
 * 背光脚来自 board_lcd_config.h（当前 = GPIO24 / SD_CLK，因硬件短接） */
#define LCD_PIN_BL              BOARD_LCD_BL_GPIO
#define LCD_PIN_RES             BOARD_LCD_RES_GPIO

/* ==================== 颜色（内存字节序 B,G,R，与驱动 BGR24 一致）==================== */
typedef struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
} lcd_color888_t;

/** 用 R,G,B 三个分量构造颜色（注意内部存成 B,G,R） */
#define LCD_COLOR888_RGB(r, g, b)   ((lcd_color888_t){ (b), (g), (r) })

#define LCD_COLOR_BLACK         LCD_COLOR888_RGB(0x00, 0x00, 0x00)
#define LCD_COLOR_RED           LCD_COLOR888_RGB(0xFF, 0x00, 0x00)
#define LCD_COLOR_GREEN         LCD_COLOR888_RGB(0x00, 0xFF, 0x00)
#define LCD_COLOR_BLUE          LCD_COLOR888_RGB(0x00, 0x00, 0xFF)
#define LCD_COLOR_WHITE         LCD_COLOR888_RGB(0xFF, 0xFF, 0xFF)
#define LCD_COLOR_YELLOW        LCD_COLOR888_RGB(0xFF, 0xFF, 0x00)
#define LCD_COLOR_CYAN          LCD_COLOR888_RGB(0x00, 0xFF, 0xFF)
#define LCD_COLOR_MAGENTA       LCD_COLOR888_RGB(0xFF, 0x00, 0xFF)
#define LCD_COLOR_GRAY          LCD_COLOR888_RGB(0x80, 0x80, 0x80)
#define LCD_COLOR_ORANGE        LCD_COLOR888_RGB(0xFF, 0x80, 0x00)
#define LCD_COLOR_PURPLE        LCD_COLOR888_RGB(0x80, 0x00, 0xFF)

/* ==================== 面板参数结构体 ==================== */
typedef struct {
    uint8_t hsync_idle_low;
    uint8_t vsync_idle_low;
    uint8_t de_idle_high;
    uint8_t pclk_active_neg;   /* 1 = 下降沿锁存数据 */
    uint8_t pclk_idle_high;
} lcd_timing_flags_t;

#define LCD_NUM_FBS     3

typedef struct {
    uint8_t *lcd_buf[LCD_NUM_FBS];
    uint8_t *front;             // LCD 正在扫描的显存
    uint8_t *ready;             // 最新解完待显示的显存
    uint8_t *back;              // JPEG 正在写入的显存
    volatile bool     new_frame;         // 是否有未上屏的新帧
    portMUX_TYPE spinlock;      // 保护指针切换的微秒级自旋锁
    QueueHandle_t jpeg_queue;
} lcd_buf_t;

typedef struct {
    uint16_t height;
    uint16_t width;
    uint8_t  pixsize;          /* 24 = RGB888 */
    uint32_t clk;              /* PCLK 频率 Hz */

    /* 水平方向 */
    uint8_t hspw;
    uint8_t hbp;
    uint8_t hfp;

    /* 垂直方向 */
    uint8_t vspw;
    uint8_t vbp;
    uint8_t vfp;

    lcd_buf_t          lcd_buf;
    uint8_t            lcd_buf_num;
    lcd_timing_flags_t timing_flag;
} lcd_info_t;

/** 面板参数（定义在 lcd.c，可在 lcd_init() 之前修改） */
extern lcd_info_t lcd;

/* ==================== 初始化 ==================== */
esp_err_t lcd_init(void);


/* ==================== 背光 / 复位 ==================== */
void lcd_backlight(bool on);
void lcd_reset_panel(void);

void lcd_get_frame(uint16_t *frame);
#endif /* LCD_H */
