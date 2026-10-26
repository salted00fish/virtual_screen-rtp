#ifndef USB_H
#define USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 TinyUSB 设备栈，并把 CDC 接口注册为标准控制台
 *
 * 流程：tinyusb_driver_install() -> tinyusb_cdcacm_init() -> tinyusb_console_init()
 * 完成后 printf / ESP_LOGI 等输出都会走 USB CDC，主机打开该串口即可看到。
 *
 * @return ESP_OK 成功，其它为错误码
 */
esp_err_t usb_serial_init(void);

/**
 * @brief 底层直接发送一段数据（不经过 stdio/VFS）
 *
 * 适合在控制台尚未就绪、或需要明确知道有没有写出去的场景使用。
 *
 * @param[in] data 数据指针
 * @param[in] len  数据长度
 * @return 实际写入的字节数
 */
size_t usb_serial_write(const uint8_t *data, size_t len);

/**
 * @brief 底层直接发送一个以 '\0' 结尾的字符串
 *
 * @param[in] str 字符串
 * @return 实际写入的字节数
 */
size_t usb_serial_write_str(const char *str);

/**
 * @brief 查询 USB 主机是否已打开串口（DTR 是否置位）
 */
bool usb_serial_connected(void);

/**
 * @brief 阻塞等待主机打开串口
 *
 * 串口工具/终端一打开端口就会置位 DTR，此时本函数返回。
 * 建议在 usb_serial_init() 之后、任何耗时初始化或日志输出之前调用，
 * 否则主机还没开始读，USB CDC 的 TX FIFO 满了会直接丢数据，
 * 表现为"打开串口后过了很久才看到日志"。
 *
 * @param[in] timeout_ms 超时时间，单位毫秒
 * @return true 主机已打开串口；false 超时（没有主机也照常继续）
 */
bool usb_serial_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* USB_H */
