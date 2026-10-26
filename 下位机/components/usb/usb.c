#include "usb.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"

// USB CDC 控制台与 USB-Serial-JTAG 共用同一个物理 USB 口，二者只能取其一。
// 你的板子只引出了 USB 2.0 OTG（专用 PHY 44/45），所以日志必须由应用自己
// 通过 TinyUSB CDC 输出（CONFIG_TINYUSB_CDC_ENABLED=y）。
// 若以后改用 USJ（GPIO33/34）输出，请关掉 CONFIG_TINYUSB_CDC_ENABLED，
// 并不要在 app_main 里调用 usb_serial_init()。
#if CONFIG_TINYUSB_CDC_ENABLED

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

static const char *TAG = "usb_serial";

// 使用哪一路 CDC 接口（CONFIG_TINYUSB_CDC_COUNT 为 1，所以只有 0）
#define USB_CDC_ITF         TINYUSB_CDC_ACM_0

static volatile bool s_connected;   // 主机是否已打开串口（DTR）

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_connected = event->line_state_changed_data.dtr;
    ESP_LOGI(TAG, "host %s the port", s_connected ? "opened" : "closed");
}

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;

    uint8_t buf[64];
    size_t rx_size = 0;

    if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_size) != ESP_OK) {
        return;
    }
    if (rx_size == 0) {
        return;
    }

    // 原样回显，方便确认双向通信正常
    tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf, buf, rx_size);
    tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, 0);
}

esp_err_t usb_serial_init(void)
{
    // 1. 安装 TinyUSB 设备栈（描述符用 esp_tinyusb 提供的默认值）
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg),
                        TAG, "tinyusb_driver_install failed");

    // 2. 初始化 CDC ACM 接口并注册回调
    const tinyusb_config_cdcacm_t cdc_cfg = {
        .cdc_port = USB_CDC_ITF,
        .callback_rx = cdc_rx_cb,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = cdc_line_state_cb,
        .callback_line_coding_changed = NULL,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&cdc_cfg), TAG, "tinyusb_cdcacm_init failed");

    // 3. 把 stdout/stdin 重定向到这个 CDC 接口，printf/ESP_LOG 之后就走 USB
    ESP_RETURN_ON_ERROR(tinyusb_console_init(USB_CDC_ITF), TAG, "tinyusb_console_init failed");

    ESP_LOGI(TAG, "USB CDC console ready");
    return ESP_OK;
}

size_t usb_serial_write(const uint8_t *data, size_t len)
{
    if (!tinyusb_cdcacm_initialized(USB_CDC_ITF)) {
        return 0;
    }

    size_t written = tinyusb_cdcacm_write_queue(USB_CDC_ITF, data, len);
    tinyusb_cdcacm_write_flush(USB_CDC_ITF, 0);
    return written;
}

size_t usb_serial_write_str(const char *str)
{
    if (str == NULL) {
        return 0;
    }
    return usb_serial_write((const uint8_t *)str, strlen(str));
}

bool usb_serial_connected(void)
{
    return s_connected;
}

bool usb_serial_wait_connected(uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();

    while (!s_connected) {
        if (timeout_ms != 0 && (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

#else  /* !CONFIG_TINYUSB_CDC_ENABLED : USB 相关接口停用 */

esp_err_t usb_serial_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

size_t usb_serial_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return 0;
}

size_t usb_serial_write_str(const char *str)
{
    (void)str;
    return 0;
}

bool usb_serial_connected(void)
{
    return false;
}

bool usb_serial_wait_connected(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return false;
}

#endif /* CONFIG_TINYUSB_CDC_ENABLED */
