#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char* TAG = "ML_RX";
static constexpr uint16_t ML_RX_PORT = 7778;

extern "C" void heart_monitor_set_bpm_ml(int bpm_ml);

static void ml_rx_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(nullptr);
        return;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ML_RX_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Listening for corrected BPM on UDP %u", (unsigned)ML_RX_PORT);

    char buf[128];

    while (true) {
        sockaddr_in from = {};
        socklen_t fromlen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromlen);
        if (len <= 0) continue;

        buf[len] = '\0';

        // Expected: t_ms,bpm_corr
        long long t_ms = 0;
        int bpm_corr = 0;

        if (std::sscanf(buf, "%lld,%d", &t_ms, &bpm_corr) == 2) {
            if (bpm_corr > 0 && bpm_corr < 260) {
                heart_monitor_set_bpm_ml(bpm_corr);
            }
        }
    }
}

extern "C" void ml_rx_start()
{
    xTaskCreate(ml_rx_task, "ml_rx", 4096, nullptr, 3, nullptr);
}
