#include "wifi_telemetry.h"

#include <cstring>
#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"


#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "heart_monitor_types.h"

// ---- EDIT THESE ----
static constexpr const char* WIFI_SSID = "rzg-88316";
static constexpr const char* WIFI_PASS = "S4Tj-4RGd-WyNA-n8hL";

// PC IP on the same network (example: 192.168.1.50). You must set this correctly.
static constexpr const char* UDP_DEST_IP = "192.168.1.151";
static constexpr uint16_t    UDP_DEST_PORT = 7777;

// How often to transmit if nothing changes (ms)
static constexpr int TELEMETRY_PERIOD_MS = 200; // 5 Hz

// --------------------
static const char* TAG = "WIFI_TLM";

static EventGroupHandle_t s_wifi_event_group;
static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAIL_BIT      = BIT1;

static int s_retry_num = 0;
static constexpr int WIFI_MAX_RETRY = 10;

// These are defined in heart_monitor_tasks.cpp; we expose them via weak getters below.
extern "C" {
    // Provide these two functions in heart_monitor_tasks.cpp (I show how in section 2).
    bool heart_monitor_peek_bpm(BpmReading* out);
    bool heart_monitor_peek_alarm(AlarmEvent* out);
}

static void wifi_event_handler(void*,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying (%d/%d)...", s_retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t* event = (const ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        return;
    }
}

bool wifi_init_sta_blocking()
{
    // NVS (required by Wi-Fi)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));

    wifi_config_t wifi_config = {};
    std::snprintf((char*)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    std::snprintf((char*)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASS);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi SSID='%s' ...", WIFI_SSID);

    // Wait up to 20 seconds
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(20000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected.");
        return true;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi failed to connect.");
        return false;
    }

    ESP_LOGE(TAG, "Wi-Fi connect timeout.");
    return false;
}

static void telemetry_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(UDP_DEST_PORT);
    dest.sin_addr.s_addr = inet_addr(UDP_DEST_IP);

    ESP_LOGI(TAG, "Telemetry UDP -> %s:%u", UDP_DEST_IP, (unsigned)UDP_DEST_PORT);

    int64_t last_sent_bpm_t_ms = -1;

    while (true) {
        BpmReading br{};
        if (heart_monitor_peek_bpm(&br)) {
            // Only send when a NEW reading arrives (timestamp changed)
            if (br.t_ms != last_sent_bpm_t_ms) {
                last_sent_bpm_t_ms = br.t_ms;

                // Optional: include alarm type snapshot at the same time
                AlarmEvent ae{};
                AlarmType alarm = AlarmType::NONE;
                if (heart_monitor_peek_alarm(&ae)) alarm = ae.type;

                // Format: sample_t_ms,bpm,quality,stable,alarm_type
                char msg[96];
                std::snprintf(msg, sizeof(msg), "%lld,%d,%.3f,%u,%u\n",
                              (long long)br.t_ms,
                              br.bpm,
                              (double)br.quality,
                              (unsigned)(br.stable ? 1 : 0),
                              (unsigned)alarm);

                int sent = sendto(sock, msg, std::strlen(msg), 0, (sockaddr*)&dest, sizeof(dest));
                if (sent < 0) {
                    ESP_LOGW(TAG, "sendto() failed");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20)); // light polling; does NOT resend old values
    }
}


void telemetry_start()
{
    xTaskCreate(telemetry_task, "telemetry_udp", 4096, nullptr, 4, nullptr);
}
