#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_bit_defs.h"

#define WIFI_CONNECTED_BIT          BIT0
/**
 * @brief 把 Wi-Fi 初始化成 STA（station）并连接路由器
 *
 * 内部流程：
 *   nvs_flash_init() -> esp_netif_init() -> esp_event_loop_create_default()
 *   -> esp_netif_create_default_wifi_sta() -> esp_wifi_init()
 *   -> 注册 WIFI_EVENT / IP_EVENT 回调 -> esp_wifi_set_mode(WIFI_MODE_STA)
 *   -> esp_wifi_set_config() -> esp_wifi_start() -> 等拿到 IP
 *
 * 路由器名字/密码在 wifi.c 顶部的 WIFI_SSID / WIFI_PASSWORD 宏里改。
 * 连接成功后会一直保持：掉线由事件回调自动重连，不需要应用层干预。
 *
 * 本函数会阻塞等待拿到 IP，最多 WIFI_CONNECT_TIMEOUT_MS（默认 15 秒）：
 *   - 拿到 IP：返回 ESP_OK
 *   - 超时：返回 ESP_ERR_TIMEOUT（此时 Wi-Fi 仍在后台重连，之后再用
 *     wifi_is_connected() / wifi_wait_connected() 查询即可）
 * 重复调用是安全的，第二次起只做"等待连接结果"。
 *
 * @return ESP_OK 已连上并拿到 IP；其它为错误码
 */
esp_err_t wifi_init(void);

/**
 * @brief 查询当前是否已连上路由器并拿到 IP
 */
bool wifi_is_connected(void);

/**
 * @brief 等待连上路由器（拿到 IP）
 *
 * @param[in] timeout_ms 最长等待时间，单位毫秒；0 表示只查一次不等待
 * @return ESP_OK 已连上；ESP_ERR_TIMEOUT 超时；ESP_ERR_INVALID_STATE 还没调用过 wifi_init()
 */
esp_err_t wifi_wait_connected(uint32_t timeout_ms);

/**
 * @brief 取当前 IP 的字符串形式，例如 "192.168.1.123"
 *
 * 未连接时返回 "0.0.0.0"。返回的是内部静态缓冲区，只读、不要 free，
 * 适合直接丢给 ESP_LOGI("%s", ...) 打印。
 */
const char *wifi_get_ip_str(void);
void *get_wifi_eventgroup(void);

#endif /* WIFI_H */
