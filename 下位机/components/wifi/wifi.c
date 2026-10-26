#include "wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"


/* ===================== 用户配置：只改这一段 ===================== */

/* 路由器的名字（SSID）和密码 */
#define WIFI_SSID                   "CMCC-mFua"
#define WIFI_PASSWORD               "py7tus72"

/* 最低可接受的加密等级：家用路由一般是 WPA2-PSK，不要往下降 */
#define WIFI_AUTH_THRESHOLD         WIFI_AUTH_WPA2_PSK

/* wifi_init() 等拿到 IP 的最长时间，单位毫秒 */
#define WIFI_CONNECT_TIMEOUT_MS     15000

/* 掉线后隔多久重连一次，单位毫秒 */
#define WIFI_RECONNECT_DELAY_MS     2000

/* ============================================================== */


static const char *TAG = "wifi";

static EventGroupHandle_t s_event_group   = NULL;   /* 给 wifi_init() 等 IP 用 */
static esp_netif_t       *s_sta_netif     = NULL;   /* STA 网卡对象 */
static esp_timer_handle_t s_reconnect_tmr = NULL;   /* 掉线重连定时器 */
static volatile bool      s_connected     = false;  /* 是否已连上并拿到 IP */
static bool               s_inited        = false;  /* 是否已经初始化过 */
static char               s_ip_str[16]    = "0.0.0.0";

/* ------------------------------------------------------------------ */
/* 定时器回调：在任务上下文里发起重连，避免在事件回调里阻塞事件循环    */
/* ------------------------------------------------------------------ */
static void wifi_reconnect_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "重新连接 \"%s\" ...", WIFI_SSID);
    esp_wifi_connect();
}

/* ------------------------------------------------------------------ */
/* WIFI_EVENT / IP_EVENT 统一回调                                      */
/* ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA 已启动，开始连接 \"%s\"", WIFI_SSID);
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = (wifi_event_sta_connected_t *)data;
        ESP_LOGI(TAG, "已关联 AP：%s（信道 %d, BSSID " MACSTR ", authmode %d）",
                 WIFI_SSID, e->channel, MAC2STR(e->bssid), (int)e->authmode);

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;

        s_connected = false;
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        }

        ESP_LOGW(TAG, "与 \"%s\" 断开（reason=%d），%d ms 后自动重连",
                 WIFI_SSID, (int)e->reason, WIFI_RECONNECT_DELAY_MS);

        if (s_reconnect_tmr && !esp_timer_is_active(s_reconnect_tmr)) {
            esp_err_t err = esp_timer_start_once(s_reconnect_tmr,
                                                 (uint64_t)WIFI_RECONNECT_DELAY_MS * 1000);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "启动重连定时器失败：%s", esp_err_to_name(err));
                esp_wifi_connect();     /* 兜底：直接重连 */
            }
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;

        esp_ip4addr_ntoa(&e->ip_info.ip, s_ip_str, sizeof(s_ip_str));
        s_connected = true;

        if (s_reconnect_tmr && esp_timer_is_active(s_reconnect_tmr)) {
            esp_timer_stop(s_reconnect_tmr);
        }
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "连接成功：IP=%s, 网关=" IPSTR ", 掩码=" IPSTR ", RSSI=%d dBm, 信道=%d",
                     s_ip_str,
                     IP2STR(&e->ip_info.gw), IP2STR(&e->ip_info.netmask),
                     ap.rssi, ap.primary);

            /* 诊断用：AP 到底给了我们什么能力 */
            ESP_LOGI(TAG, "AP能力: 11b=%d 11g=%d 11n=%d, LR=%d, bandwidth=%d(1=HT20,2=HT40)",
                     ap.phy_11b, ap.phy_11g, ap.phy_11n,
                     ap.phy_lr, (int)ap.bandwidth);
        } else {
            ESP_LOGI(TAG, "连接成功：IP=%s, 网关=" IPSTR, s_ip_str, IP2STR(&e->ip_info.gw));
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        ESP_LOGW(TAG, "IP 已丢失（等待重连或 DHCP 续约）");
    }
}

/* ------------------------------------------------------------------ */
/* 公共部分：网络接口 + 默认事件循环（重复调用不算错）                  */
/* ------------------------------------------------------------------ */
static esp_err_t wifi_netif_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default 失败：%s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */
esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_connected) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                          WIFI_CONNECTED_BIT,
                                          pdFALSE,      /* 不清除标志位 */
                                          pdTRUE,       /* 等待全部位 */
                                          pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

const char *wifi_get_ip_str(void)
{
    return s_ip_str;
}

esp_err_t wifi_init(void)
{
    esp_err_t err;

    /* 已经初始化过了：只报告连接结果，避免重复 esp_wifi_init */
    if (s_inited)
    {
        ESP_LOGI(TAG, "wifi_init() 已执行过，直接等待连接结果");
        return wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
    }

    /* ---- 2. 网络接口 + 默认事件循环 ---- */
    ESP_RETURN_ON_ERROR(wifi_netif_init(), TAG, "网络接口/事件循环初始化失败");

    /* ---- 3. 等 IP 用的事件组 ---- */
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        if (s_event_group == NULL) {
            ESP_LOGE(TAG, "创建事件组失败（内存不足）");
            return ESP_ERR_NO_MEM;
        }
    }

    /* ---- 4. 创建默认 STA 网卡（DHCP 客户端由它负责） ---- */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "创建默认 STA 网卡失败");
        return ESP_FAIL;
    }

    /* ---- 5. 初始化 Wi-Fi 驱动 ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init 失败");

    /* ---- 6. 注册事件：Wi-Fi 状态 + 拿到/丢失 IP ---- */
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 WIFI_EVENT 处理函数失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP_EVENT_STA_GOT_IP 处理函数失败");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "注册 IP_EVENT_STA_LOST_IP 处理函数失败");

    /* ---- 7. 掉线重连定时器 ---- */
    const esp_timer_create_args_t tmr_args = {
        .callback = wifi_reconnect_cb,
        .arg      = NULL,
        .name     = "wifi_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tmr_args, &s_reconnect_tmr), TAG, "创建重连定时器失败");

    /* ---- 8. STA 配置：SSID / 密码 / 加密门槛 ---- */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
            /* 比 WIFI_AUTH_THRESHOLD 更弱的 AP 不连（防止误连开放热点） */
            .threshold.authmode = WIFI_AUTH_THRESHOLD,
            /* WPA3 的 SAE 支持 Hunt&Peck 和 H2E 两种方式，两种都开着兼容性最好 */
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            /* 路由器支持 PMF 时使用，但不强制，保证 WPA2 路由也能连 */
            .pmf_cfg = {
                .capable  = true,
                .required = false,
            },
        },
    };

    wifi_country_t country = {
        .cc = "CN",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,      // dBm，20 是 ESP32 的最大合法值
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_country(&country),TAG,"设置国家码失败");

    /* 配置只放内存，不写进 NVS：换路由器只改宏，不会被上次的旧配置顶掉 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "设置 Wi-Fi 存储方式失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "设置 WIFI_MODE_STA 失败");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "写入 STA 配置失败");

    /* ---- 9. 启动：STA_START 事件里会自动发起连接 ---- */
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start 失败");

    /* 视频流场景：关掉 Wi-Fi 省电模式。
     * 默认的 WIFI_PS_MIN_MODEM 会把下行包攒到 DTIM 才一起发，推流时表现为
     * "一阵一阵的突发 + 延迟抖动"，配合 lwip 的 UDP 邮箱很容易整帧丢掉。
     * 想省电就把这行改回 WIFI_PS_MIN_MODEM。 */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "esp_wifi_set_ps 失败");

    s_inited = true;

    ESP_LOGI(TAG, "Wi-Fi STA 初始化完成，等待连接 \"%s\"（最多 %d ms）",
             WIFI_SSID, WIFI_CONNECT_TIMEOUT_MS);

    /* ---- 10. 等拿到 IP ---- */
    err = wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi 已就绪，IP=%s", s_ip_str);
    } else {
        ESP_LOGW(TAG, "等待连接超时（%d ms）：检查 SSID/密码/信号后会自动重连，"
                      "可用 wifi_is_connected() 查询", WIFI_CONNECT_TIMEOUT_MS);
    }

    return err;
}


void *get_wifi_eventgroup(void)
{
    return s_event_group;
}