#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include "driver/gpio.h"

static const char *TAG = "WIFI_AP_CONFIG_FINAL";

// ========== LED配置 (低电平点亮，外接LED推荐共阳极接法) ==========
#define LED_STATUS_GPIO   12    // WiFi状态灯
#define LED_RUN_GPIO      13    // 运行指示灯

// ========== WiFi事件通知位定义 ==========
#define WIFI_EVENT_BIT_CONNECTED    (1 << 0)
#define WIFI_EVENT_BIT_DISCONNECTED (1 << 1)
#define WIFI_EVENT_BIT_RESTART      (1 << 2)

// WiFi配置存储
#define WIFI_CONFIG_NAMESPACE "wifi_config"
#define WIFI_SSID_KEY "ssid"
#define WIFI_PWD_KEY "password"

// WiFi重连配置
#define WIFI_MAX_RETRY 5
#define WIFI_MGR_TASK_PRIORITY 6
#define WIFI_MGR_TASK_STACK 8192

// WiFi状态
static volatile int g_wifi_retry_cnt = 0;
static char g_sta_ssid[33] = {0};
static char g_sta_pwd[65] = {0};
static volatile bool g_sta_connected = false;

// Web服务器与网络接口
static httpd_handle_t g_web_server = NULL;
static esp_netif_t *g_sta_netif = NULL;
static esp_netif_t *g_ap_netif = NULL;

// WiFi管理任务句柄
static TaskHandle_t g_wifi_mgr_task_handle = NULL;

// ========== 前置声明 ==========
static void start_ap_mode(void);
static void start_sta_mode(void);
static esp_err_t start_web_server(bool enable_ws);
static esp_err_t stop_web_server(void);
static void wifi_mgr_task(void *arg);

// ========== 配网网页HTML ==========
static const char *html_page = "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP32 配网</title>"
    "<style>"
    "body { font-family: sans-serif; background: #f0f2f5; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }"
    ".card { background: white; padding: 2rem; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.1); width: 100%; max-width: 350px; }"
    "h2 { color: #1a73e8; margin-top: 0; text-align: center; }"
    "label { display: block; margin-bottom: 5px; color: #5f6368; }"
    "input { width: 100%; padding: 10px; margin-bottom: 20px; border: 1px solid #dadce0; border-radius: 4px; box-sizing: border-box; }"
    "button { width: 100%; padding: 12px; background: #1a73e8; color: white; border: none; border-radius: 4px; font-size: 1rem; cursor: pointer; }"
    "</style></head><body>"
    "<div class=\"card\">"
    "<h2>WiFi 快速配置</h2>"
    "<form action=\"/save\" method=\"POST\" id=\"wifiForm\">"
    "<label>WiFi 名称 (SSID)</label>"
    "<input type=\"text\" name=\"ssid\" required>"
    "<label>WiFi 密码</label>"
    "<input type=\"password\" name=\"pwd\" required>"
    "<button type=\"submit\">保存并重启连接</button>"
    "</form></div></body></html>";

static const char *html_404 = "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>";

// ========== WebSocket 处理函数 (仅STA模式启用) ==========
#ifdef CONFIG_HTTPD_WS_SUPPORT
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket客户端连接成功");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t buf[256];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = buf;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket接收失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        buf[ws_pkt.len] = '\0';
        ESP_LOGI(TAG, "收到WebSocket消息: %s", buf);

        char resp[300];
        snprintf(resp, sizeof(resp), "ESP32已收到: %s", buf);
        httpd_ws_frame_t resp_pkt = {
            .payload = (uint8_t *)resp,
            .len = strlen(resp),
            .type = HTTPD_WS_TYPE_TEXT
        };
        httpd_ws_send_frame(req, &resp_pkt);
    }
    return ESP_OK;
}
#endif

// ========== LED控制函数 ==========
static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_STATUS_GPIO) | (1ULL << LED_RUN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    // 默认熄灭（低电平点亮，输出高电平=熄灭）
    gpio_set_level(LED_STATUS_GPIO, 1);
    gpio_set_level(LED_RUN_GPIO, 1);
}

static void led_status_task(void *arg) {
    led_init();
    ESP_LOGI(TAG, "LED状态指示初始化完成 (GPIO:%d, GPIO:%d)", LED_STATUS_GPIO, LED_RUN_GPIO);

    bool toggle = false;
    while (1) {
        if (!g_sta_connected) {
            // 未连接：两灯交替闪烁
            if (toggle) {
                gpio_set_level(LED_STATUS_GPIO, 0); // 亮
                gpio_set_level(LED_RUN_GPIO, 1);    // 灭
            } else {
                gpio_set_level(LED_STATUS_GPIO, 1); // 灭
                gpio_set_level(LED_RUN_GPIO, 0);    // 亮
            }
            toggle = !toggle;
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            // 连接成功：双灯常亮
            gpio_set_level(LED_STATUS_GPIO, 0);
            gpio_set_level(LED_RUN_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// ========== NVS配置读写 ==========
static esp_err_t wifi_config_save(const char *ssid, const char *pwd) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_str(nvs_handle, WIFI_SSID_KEY, ssid);
    if (ret == ESP_OK) ret = nvs_set_str(nvs_handle, WIFI_PWD_KEY, pwd);
    if (ret == ESP_OK) ret = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    return ret;
}

static esp_err_t wifi_config_load(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) return ret;
    size_t ssid_len = sizeof(g_sta_ssid);
    size_t pwd_len = sizeof(g_sta_pwd);
    ret = nvs_get_str(nvs_handle, WIFI_SSID_KEY, g_sta_ssid, &ssid_len);
    if (ret == ESP_OK) ret = nvs_get_str(nvs_handle, WIFI_PWD_KEY, g_sta_pwd, &pwd_len);
    nvs_close(nvs_handle);
    return ret;
}

static void url_decode(char *dest, const char *src, int max_len) {
    int i = 0, j = 0;
    while (src[i] != '\0' && j < max_len - 1) {
        if (src[i] == '%' && src[i+1] != '\0' && src[i+2] != '\0') {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dest[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dest[j++] = ' ';
            i++;
        } else {
            dest[j++] = src[i++];
        }
    }
    dest[j] = '\0';
}

// ========== HTTP回调 ==========
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    return httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t not_found_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "text/html; charset=UTF-8");
    return httpd_resp_send(req, html_404, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }

    size_t total_len = req->content_len;
    if (total_len == 0 || total_len >= 255) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "请求体长度非法");
    }

    char buf[256];
    size_t received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "数据接收失败");
        }
        received += ret;
    }
    buf[received] = '\0';

    char ssid_raw[33] = {0}, pwd_raw[65] = {0};
    char ssid[33] = {0}, pwd[65] = {0};

    char *p_ssid = strstr(buf, "ssid=");
    if (!p_ssid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少SSID");
    p_ssid += 5;
    char *p_sep = strchr(p_ssid, '&');
    size_t slen = p_sep ? (size_t)(p_sep - p_ssid) : strlen(p_ssid);
    if (slen >= sizeof(ssid_raw)) slen = sizeof(ssid_raw) - 1;
    memcpy(ssid_raw, p_ssid, slen);

    char *p_pwd = strstr(buf, "pwd=");
    if (!p_pwd) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少密码");
    p_pwd += 4;
    p_sep = strchr(p_pwd, '&');
    size_t plen = p_sep ? (size_t)(p_sep - p_pwd) : strlen(p_pwd);
    if (plen >= sizeof(pwd_raw)) plen = sizeof(pwd_raw) - 1;
    memcpy(pwd_raw, p_pwd, plen);

    url_decode(ssid, ssid_raw, sizeof(ssid));
    url_decode(pwd, pwd_raw, sizeof(pwd));

    if (strlen(ssid) == 0 || strlen(ssid) > 32) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID长度1~32");
    }
    if (strlen(pwd) > 0 && strlen(pwd) < 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "密码至少8位");
    }

    if (wifi_config_save(ssid, pwd) == ESP_OK) {
        httpd_resp_set_type(req, "text/html; charset=UTF-8");
        httpd_resp_sendstr(req, "<h3>保存成功，设备即将重启</h3>");
        if (g_wifi_mgr_task_handle) {
            xTaskNotify(g_wifi_mgr_task_handle, WIFI_EVENT_BIT_RESTART, eSetBits);
        }
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "保存失败");
    }
    return ESP_OK;
}

// ========== Web服务器启停 (已修复WebSocket注册警告) ==========
static esp_err_t start_web_server(bool enable_ws) {
    if (g_web_server != NULL) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 5;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&g_web_server, &config) != ESP_OK) return ESP_FAIL;

    // 优先注册精确的WebSocket路由，避免通配符路由匹配冲突
#ifdef CONFIG_HTTPD_WS_SUPPORT
    if (enable_ws) {
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(g_web_server, &ws_uri);
        ESP_LOGI(TAG, "✅ WebSocket接口已启用");
    }
#endif

    // 再注册基础HTTP路由，通配符放在最后
    httpd_uri_t base_uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/save", .method = HTTP_POST, .handler = save_handler },
        { .uri = "/save", .method = HTTP_GET, .handler = save_handler },
        { .uri = "/*", .method = HTTP_GET, .handler = not_found_handler }
    };
    for (int i = 0; i < sizeof(base_uris) / sizeof(base_uris[0]); i++) {
        httpd_register_uri_handler(g_web_server, &base_uris[i]);
    }

    ESP_LOGI(TAG, "✅ Web服务器启动完成");
    return ESP_OK;
}

static esp_err_t stop_web_server(void) {
    if (g_web_server && httpd_stop(g_web_server) == ESP_OK) {
        g_web_server = NULL;
    }
    return ESP_OK;
}

// ========== WiFi事件回调 (500ms防抖) ==========
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    static TickType_t last_disconnect_tick = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_sta_connected = false;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_disconnect_tick) > pdMS_TO_TICKS(500)) {
            last_disconnect_tick = now;
            if (g_wifi_mgr_task_handle) {
                xTaskNotify(g_wifi_mgr_task_handle, WIFI_EVENT_BIT_DISCONNECTED, eSetBits);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✅ 获取IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_sta_connected = true;
        if (g_wifi_mgr_task_handle) {
            xTaskNotify(g_wifi_mgr_task_handle, WIFI_EVENT_BIT_CONNECTED, eSetBits);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        if (g_ap_netif) {
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(g_ap_netif, &ip_info);
            ESP_LOGI(TAG, "✅ AP模式已启动: http://" IPSTR, IP2STR(&ip_info.ip));
        }
    }
}

// ========== 模式启停 ==========
static void start_ap_mode(void) {
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t mac[6] = {0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99};
    esp_efuse_mac_get_default(mac);
    char ap_pwd[16];
    snprintf(ap_pwd, sizeof(ap_pwd), "ESP_%02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "🔑 AP密码: %s", ap_pwd);

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, "ESP32-C3-Config", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen("ESP32-C3-Config");
    strncpy((char *)ap_cfg.ap.password, ap_pwd, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = 4;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
}

static void start_sta_mode(void) {
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    g_wifi_retry_cnt = 0;
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, g_sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, g_sta_pwd, sizeof(sta_cfg.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    esp_err_t ret = esp_wifi_start();
    if (ret == ESP_OK) {
        ret = esp_wifi_connect();
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STA 启动/连接失败: %s", esp_err_to_name(ret));
        if (g_wifi_mgr_task_handle) {
            xTaskNotify(g_wifi_mgr_task_handle, WIFI_EVENT_BIT_DISCONNECTED, eSetBits);
        }
    }
}

// ========== 核心：WiFi管理任务 ==========
static void wifi_mgr_task(void *arg) {
    uint32_t notify_val = 0;
    bool ap_mode_active = false;

    if (strlen(g_sta_ssid) == 0) {
        ESP_LOGW(TAG, "无 WiFi 配置，直接进入 AP 模式");
        ap_mode_active = true;
        start_ap_mode();
        start_web_server(false); // AP模式不开启WebSocket
    } else {
        ESP_LOGI(TAG, "读取到保存的 WiFi: %s，开始连接", g_sta_ssid);
        start_sta_mode();
    }

    while (1) {
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &notify_val, pdMS_TO_TICKS(10000)) == pdTRUE) {

            // 1. 最高优先级：重启指令
            if (notify_val & WIFI_EVENT_BIT_RESTART) {
                ESP_LOGI(TAG, "收到重启指令，准备重启...");
                stop_web_server();
                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }

            // 2. 连接成功
            if (notify_val & WIFI_EVENT_BIT_CONNECTED) {
                ESP_LOGI(TAG, "STA 连接成功，启动Web服务");
                g_wifi_retry_cnt = 0;
                ap_mode_active = false;
                start_web_server(true); // STA模式开启WebSocket
            }

            // 3. 断开连接
            if (notify_val & WIFI_EVENT_BIT_DISCONNECTED) {
                if (!ap_mode_active) {
                    stop_web_server();
                    g_wifi_retry_cnt++;
                    ESP_LOGW(TAG, "STA 断开，重试次数: %d/%d", g_wifi_retry_cnt, WIFI_MAX_RETRY);

                    if (g_wifi_retry_cnt <= WIFI_MAX_RETRY) {
                        int delay_ms = 5000 * (1 << (g_wifi_retry_cnt - 1));
                        if (delay_ms > 60000) delay_ms = 60000;
                        ESP_LOGI(TAG, "等待 %d ms 后重试 (可中断)...", delay_ms);

                        bool break_delay = false;
                        for (int i = 0; i < delay_ms; i += 500) {
                            vTaskDelay(pdMS_TO_TICKS(500));
                            uint32_t temp_val = 0;
                            if (xTaskNotifyWait(0, ULONG_MAX, &temp_val, 0) == pdTRUE) {
                                if (temp_val & (WIFI_EVENT_BIT_CONNECTED | WIFI_EVENT_BIT_RESTART)) {
                                    xTaskNotify(g_wifi_mgr_task_handle, temp_val, eSetBits);
                                    break_delay = true;
                                    break;
                                }
                            }
                        }
                        if (break_delay) {
                            continue;
                        }

                        esp_err_t ret = esp_wifi_connect();
                        if (ret != ESP_OK) {
                            ESP_LOGE(TAG, "重试调用 esp_wifi_connect 失败: %s", esp_err_to_name(ret));
                            xTaskNotify(g_wifi_mgr_task_handle, WIFI_EVENT_BIT_DISCONNECTED, eSetBits);
                        }
                    } else {
                        ESP_LOGW(TAG, "重试耗尽，切换到 AP 模式");
                        ap_mode_active = true;
                        stop_web_server();
                        start_ap_mode();
                        start_web_server(false); // AP模式不开启WebSocket
                    }
                }
            }
        } else {
            if (ap_mode_active) {
                ESP_LOGD(TAG, "AP 模式运行中，等待用户配网...");
            }
        }
    }
}

// ========== 主函数 ==========
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_load();

    xTaskCreate(led_status_task, "led_task", 4096, NULL, 5, NULL);
    xTaskCreate(wifi_mgr_task, "wifi_mgr_task", WIFI_MGR_TASK_STACK, NULL, WIFI_MGR_TASK_PRIORITY, &g_wifi_mgr_task_handle);

    ESP_LOGI(TAG, "系统初始化完成，交由 WiFi 管理任务接管...");
}

