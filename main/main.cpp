#include "esp_log.h"
#include "wifi_telemetry.h"

extern "C" void heart_monitor_start();
extern "C" void ml_rx_start();

extern "C" void app_main(void)
{
    bool ok = wifi_init_sta_blocking();
    if (!ok) {
        ESP_LOGW("MAIN", "Wi-Fi not connected; continuing without telemetry.");
    } else {
        telemetry_start();
        ml_rx_start();
    }

    heart_monitor_start();
}
