#include "i2c.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include <inttypes.h>
#include <string.h>

#define TAG "I2C"

/* ---- 引脚 ---- */
#define I2C_SCL_PIN         GPIO_NUM_22
#define I2C_SDA_PIN         GPIO_NUM_23
#define TOUCH_RST_PIN       GPIO_NUM_24

#define TOUCH_I2C_ADDR      0x14

/* ---- GT9xxx 寄存器 ---- */
#define GT_CTRL_REG         0x8040   /* 写 2=软复位，写 0=结束软复位 */
#define GT_PID_REG          0x8140   /* 产品 ID（4 字节 ASCII）*/
#define GT_GSTID_REG        0x814E   /* 状态：bit7=有数据, bit3:0=点数 */
#define GT_TP1_REG          0x8150   /* 第 1 个触点（每点 8 字节）*/
#define GT_TPN_REG_STEP     8



static i2c_master_dev_handle_t i2c_dev = NULL;

static esp_err_t gt_write_reg(uint16_t reg, const uint8_t *data, size_t len)
{
    if (i2c_dev == NULL || len > 16) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[2 + 16];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    if (len) {
        memcpy(&buf[2], data, len);
    }
    return i2c_master_transmit(i2c_dev, buf, 2 + len, 200);
}

static esp_err_t gt_read_reg(uint16_t reg, uint8_t *data, size_t len)
{
    if (i2c_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(i2c_dev, addr, 2, data, len, 200);
}

esp_err_t touch_i2c_init(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "复位 GT9xxx ...");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TOUCH_RST_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(TOUCH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));


    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = I2C_SCL_PIN,
        .sda_io_num                   = I2C_SDA_PIN,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus;
    err = i2c_new_master_bus(&bus_cfg, &bus);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "I2C 总线初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(bus, TOUCH_I2C_ADDR, 200);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "0x%02X 未找到 (%s)", TOUCH_I2C_ADDR, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "找到设备 0x%02X", TOUCH_I2C_ADDR);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TOUCH_I2C_ADDR,
        .scl_speed_hz    = 400*1000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &i2c_dev);
    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "注册 I2C 设备失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t pid[4] = {0};
    if (gt_read_reg(GT_PID_REG, pid, 4) == ESP_OK)
    {
        ESP_LOGI(TAG, "产品 ID = '%c%c%c%c'", pid[0], pid[1], pid[2], pid[3]);
    }

    uint8_t v = 2;
    gt_write_reg(GT_CTRL_REG, &v, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    v = 0;
    gt_write_reg(GT_CTRL_REG, &v, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

/* ============================================================
 * 读一次触摸（严格按官方顺序）
 *   ① 读 0x814E
 *   ② 若 bit7=1 → 立刻写 0 清标志
 *   ③ 若点数>0 → 连读 n*8 字节，解析坐标（高字节在前）
 * @return 点数（0=无触摸，-1=I2C 错误）
 * ============================================================ */
int gt_poll_once(uint16_t *xs, uint16_t *ys)
{
    // 静态变量：用于保存上一次的有效触摸点数和坐标
    static int      last_n = 0;
    static uint16_t last_xs[TOUCH_MAX_POINTS] = {0};
    static uint16_t last_ys[TOUCH_MAX_POINTS] = {0};

    uint8_t st = 0;
    if (gt_read_reg(GT_GSTID_REG, &st, 1) != ESP_OK)
    {
        ESP_LOGE(TAG, "read GT_GSTID_REG error");
        return -1;
    }

    const bool ready = (st & 0x80) != 0;
    const int  n     = st & 0x0F;

    // 1. 如果芯片还没准备好新的一帧数据 (ready == 0)
    // 直接把上一次缓存的坐标和点数交给调用者，保持状态不断触！
    if (!ready)
    {
        for (int i = 0; i < last_n; i++)
        {
            xs[i] = last_xs[i];
            ys[i] = last_ys[i];
        }
        return last_n;
    }

    // 2. 走到这里说明 ready == 1（芯片有新数据了！）
    if (n > 0 && n <= TOUCH_MAX_POINTS)
    {
        uint8_t buf[TOUCH_MAX_POINTS * GT_TPN_REG_STEP];
        if (gt_read_reg(GT_TP1_REG, buf, (size_t)n * GT_TPN_REG_STEP) != ESP_OK)
        {
            uint8_t zero = 0x00;
            gt_write_reg(GT_GSTID_REG, &zero, 1);
            return -1;
        }

        for (int i = 0; i < n; i++)
        {
            const uint8_t *p = &buf[i * GT_TPN_REG_STEP];
            last_xs[i] = ((uint16_t)p[1] << 8) | p[0];
            last_ys[i] = ((uint16_t)p[3] << 8) | p[2];
            xs[i] = last_xs[i];
            ys[i] = last_ys[i];
        }
        last_n = n; // 更新缓存点数（比如 1 变成 2）
    }
    else
    {
        // 当 ready == 1 且 n == 0 时（st = 0x80），说明手指真正离开了屏幕！
        last_n = 0;
    }

    // 3. 读完新数据后，向 0x814E 写 0x00 通知芯片准备下一帧
    uint8_t zero = 0x00;
    if (gt_write_reg(GT_GSTID_REG, &zero, 1) != ESP_OK)
    {
        ESP_LOGE(TAG, "write GT_GSTID_REG error");
        return -1;
    }

    return last_n;
}