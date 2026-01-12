// heart_monitor_tasks.cpp
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "driver/adc.h"

#include "pulse_bpm.h"
#include "heart_monitor_types.h"
#include "hr_anomaly_detector.h"
#include "lcd2004_liquidcrystal_i2c.h"

extern "C" bool heart_monitor_peek_bpm_ml(int* out_bpm_ml);
extern "C" void heart_monitor_set_bpm_ml(int bpm_ml);

static const char* TAG = "HR_TASKS";
static inline int64_t now_ms() { return esp_timer_get_time() / 1000; }

// -------------------- Pulse sensor config --------------------
// GPIO34 -> ADC1_CHANNEL_6
static constexpr adc1_channel_t   ADC_CHANNEL = ADC1_CHANNEL_6;
static constexpr adc_atten_t      ADC_ATTEN   = ADC_ATTEN_DB_12;
static constexpr adc_bits_width_t ADC_WIDTH   = ADC_WIDTH_BIT_12;

static constexpr int SAMPLE_PERIOD_MS = 10;    // 100 Hz
static constexpr int WARMUP_MS        = 1500;
static constexpr int SETTLING_TIME_MS = 1500;

static constexpr int RAW_NEAR_ZERO  = 50;
static constexpr int STEP_TRANSIENT = 600;

// -------------------- LCD config --------------------
static constexpr uint8_t    LCD_ADDR    = 0x27;       // confirmed by your scan
static constexpr gpio_num_t LCD_SDA     = GPIO_NUM_21;
static constexpr gpio_num_t LCD_SCL     = GPIO_NUM_22;
static constexpr uint32_t   I2C_FREQ_HZ = 100000;
// ------------------------------------------------------------

// Median of 5 helper (ADC spike suppression)
static inline int median5(int a, int b, int c, int d, int e)
{
    int v[5] = {a,b,c,d,e};
    std::sort(v, v+5);
    return v[2];
}

static int read_adc_median5()
{
    int a = adc1_get_raw(ADC_CHANNEL);
    int b = adc1_get_raw(ADC_CHANNEL);
    int c = adc1_get_raw(ADC_CHANNEL);
    int d = adc1_get_raw(ADC_CHANNEL);
    int e = adc1_get_raw(ADC_CHANNEL);
    return median5(a,b,c,d,e);
}

// Queues (latest-only mailbox semantics)
static QueueHandle_t g_bpm_q   = nullptr;  // BpmReading
static QueueHandle_t g_alarm_q = nullptr;  // AlarmEvent

// Treat NO_SIGNAL as STATUS (non-critical); other alarms are critical
static inline bool is_critical_alarm(AlarmType t)
{
    return (t != AlarmType::NONE && t != AlarmType::NO_SIGNAL);
}

static void print_alarm_edge(const AlarmEvent& ev)
{
    // NO_SIGNAL is status, not an alarm
    if (ev.type == AlarmType::NO_SIGNAL) {
        std::printf("[STATUS] NO_SIGNAL t=%lld\n", (long long)ev.t_ms);
        return;
    }

    if (ev.type == AlarmType::NONE) {
        std::printf("[ALARM] CLEARED t=%lld\n", (long long)ev.t_ms);
    } else {
        std::printf("[ALARM] t=%lld type=%s bpm=%d\n",
                    (long long)ev.t_ms, alarm_type_str(ev.type), ev.bpm);
    }
}

static const char* alarm_user_text(AlarmType t)
{
    switch (t) {
    case AlarmType::BRADYCARDIA:  return "HEART RATE LOW";
    case AlarmType::TACHYCARDIA:  return "HEART RATE HIGH";
    case AlarmType::RAPID_CHANGE: return "HR UNSTABLE";
    default:                      return "";
    }
}

// -------------------- Tasks --------------------
static void sampler_task(void*)
{
    adc1_config_width(ADC_WIDTH);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);

    PulseBpm estimator;
    int raw0 = read_adc_median5();
    estimator.reset(raw0);

    enum class RunState { BOOT_WARMUP, SETTLING, RUNNING };
    RunState state = RunState::BOOT_WARMUP;

    int64_t warmup_until   = now_ms() + WARMUP_MS;
    int64_t settling_until = 0;

    int last_raw = raw0;

    while (true) {
        int raw = read_adc_median5();
        int64_t t = now_ms();

        int step = std::abs(raw - last_raw);
        last_raw = raw;

        bool contact_transient = (raw < RAW_NEAR_ZERO) || (step > STEP_TRANSIENT);

        if (state == RunState::BOOT_WARMUP) {
            if (t >= warmup_until) {
                state = RunState::SETTLING;
                settling_until = t + SETTLING_TIME_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        if (contact_transient) {
            state = RunState::SETTLING;
            settling_until = t + SETTLING_TIME_MS;
        }

        if (state == RunState::SETTLING) {
            if (t >= settling_until) {
                estimator.reset(raw);
                state = RunState::RUNNING;
            }
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        // RUNNING
        int bpm = 0;
        float q = 0.0f;
        auto res = estimator.update(raw, t, bpm, q);

        if (res != PulseBpm::Result::NONE) {
            BpmReading r;
            r.bpm = bpm;
            r.quality = q;
            r.stable = (res == PulseBpm::Result::STABLE);
            r.t_ms = t;

            (void)xQueueOverwrite(g_bpm_q, &r);

            // Serial output: ONLY BPM
            std::printf("BPM=%d\n", r.bpm);
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

static void detector_task(void*)
{
    HrAnomalyDetector detector;

    BpmReading last{};
    bool have_last = false;

    while (true) {
        const int64_t tnow = now_ms();

        BpmReading r{};
        bool have_new = (xQueuePeek(g_bpm_q, &r, 0) == pdTRUE);

        if (have_new) {
            last = r;
            have_last = true;
        }

        // Tick the detector periodically so sustain/clear/no-signal timers work.
        BpmReading in{};
        if (!have_last) {
            in.bpm = 0;
            in.quality = 0.0f;
            in.stable = false;
            in.t_ms = tnow;
        } else {
            in = last;
            in.t_ms = tnow; // advance time even if bpm doesn't update

            // If last bpm is stale, treat as no signal (match default 3000ms)
            if ((tnow - last.t_ms) > 3000) {
                in.bpm = 0;
                in.quality = 0.0f;
                in.stable = false;
            }
        }

        AlarmEvent out{};
        if (detector.update(in, out)) {
            (void)xQueueOverwrite(g_alarm_q, &out);
            print_alarm_edge(out);
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz tick
    }
}

static void lcd_task(void*)
{
    Lcd2004LiquidCrystalI2c lcd(LCD_ADDR, 20, 4);

    esp_err_t err = lcd.init(LCD_SDA, LCD_SCL, I2C_FREQ_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed (%s)", esp_err_to_name(err));
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "LCD init OK addr=0x%02X SDA=%d SCL=%d", LCD_ADDR, (int)LCD_SDA, (int)LCD_SCL);

    lcd.backlight(true);
    (void)lcd.clear();

    // Custom heart character (slot 0)
    static const uint8_t HEART[8] = {
        0b00000,
        0b01010,
        0b11111,
        0b11111,
        0b11111,
        0b01110,
        0b00100,
        0b00000
    };
    (void)lcd.createChar(0, HEART);

    // Initial UI
    (void)lcd.printLine(0, "BPM: ---");
    (void)lcd.printLine(1, "Place finger");
    (void)lcd.printLine(2, "on sensor...");
    (void)lcd.printLine(3, "");

    // Freshness: if we haven't received a new BPM recently, treat as not available
    static constexpr int64_t BPM_STALE_MS = 3000;   // 3 seconds without new beat update
    static constexpr int64_t ML_STALE_MS  = 3000;   // same concept for ML bpm

    // Cached state
    bool    have_raw_bpm = false;
    int     raw_bpm = 0;
    int64_t raw_bpm_t_ms = 0;

    bool    have_ml_bpm = false;
    int     ml_bpm = 0;
    int64_t ml_bpm_rx_ms = 0; // local time when we last received ML bpm

    AlarmType alarm = AlarmType::NONE;
    int alarm_bpm = 0;

    AlarmType last_drawn_alarm = (AlarmType)255;
    int  last_drawn_shown_bpm = -1;

    // Heart blink (only in fully normal mode)
    bool heart_on = true;
    int64_t next_heart_ms = now_ms() + 600;

    // Backlight flash (only in critical alarm)
    bool bl = true;
    int64_t next_flash_ms = now_ms() + 250;

    while (true) {
        const int64_t t = now_ms();

        // ---------- Pull latest RAW BPM ----------
        BpmReading r{};
        if (xQueuePeek(g_bpm_q, &r, 0) == pdTRUE) {
            have_raw_bpm = true;
            raw_bpm = r.bpm;
            raw_bpm_t_ms = r.t_ms;
        }

        // Mark raw bpm stale if too old
        if (have_raw_bpm && (t - raw_bpm_t_ms) > BPM_STALE_MS) {
            have_raw_bpm = false;
        }

        // ---------- Pull latest ML BPM ----------
        int ml_tmp = 0;
        if (heart_monitor_peek_bpm_ml(&ml_tmp)) {
            // Basic sanity
            if (ml_tmp > 0 && ml_tmp < 260) {
                // Only update "received time" if the value actually changes
                if (!have_ml_bpm || ml_tmp != ml_bpm) {
                    ml_bpm = ml_tmp;
                    have_ml_bpm = true;
                    ml_bpm_rx_ms = t;
                }
            }
        }

        // Mark ML bpm stale if not updated recently
        if (have_ml_bpm && (t - ml_bpm_rx_ms) > ML_STALE_MS) {
            have_ml_bpm = false;
        }

        // ---------- Pull latest alarm ----------
        AlarmEvent a{};
        if (xQueuePeek(g_alarm_q, &a, 0) == pdTRUE) {
            alarm = a.type;
            alarm_bpm = a.bpm;
        }

        const bool critical_alarm = is_critical_alarm(alarm);

        // ---------- Choose what BPM to show ----------
        const bool have_shown_bpm = (have_ml_bpm || have_raw_bpm);
        const int  shown_bpm = have_ml_bpm ? ml_bpm : (have_raw_bpm ? raw_bpm : 0);

        // Heart blink only when fully normal (NONE) and bpm available
        if ((alarm == AlarmType::NONE) && have_shown_bpm && t >= next_heart_ms) {
            heart_on = !heart_on;
            next_heart_ms = t + 600;
            (void)lcd.setCursor(19, 0);
            (void)lcd.writeChar(heart_on ? 0 : ' ');
        }

        // Flash backlight only for critical alarms
        if (critical_alarm) {
            if (t >= next_flash_ms) {
                bl = !bl;
                lcd.backlight(bl);
                next_flash_ms = t + 250;
            }
        } else {
            if (!bl) {
                bl = true;
                lcd.backlight(true);
            }
        }

        // ---------- Redraw on alarm-type change ----------
        if (alarm != last_drawn_alarm) {
            last_drawn_alarm = alarm;
            (void)lcd.clear();
            last_drawn_shown_bpm = -1; // force BPM redraw after any alarm change

            if (!critical_alarm) {
                // Normal screen (includes NO_SIGNAL as status)
                (void)lcd.printLine(0, "BPM: ---");
                (void)lcd.printLine(1, "");
                (void)lcd.printLine(2, "");
                (void)lcd.printLine(3, "");
            } else {
                // Critical alarm takeover screen
                (void)lcd.printLine(0, "!!!   ALARM   !!!");
                (void)lcd.printLine(1, alarm_user_text(alarm));

                // Prefer current shown BPM if available; fallback to alarm event BPM
                const int alarm_show_bpm = have_shown_bpm ? shown_bpm : alarm_bpm;

                char l2[21];
                std::snprintf(l2, sizeof(l2), "HR: %3d bpm", alarm_show_bpm);
                (void)lcd.printLine(2, l2);
                (void)lcd.printLine(3, "PULL OVER SAFELY");
            }
        }

        // ---------- Normal/Status screen updates (NONE or NO_SIGNAL) ----------
        if (!critical_alarm) {
            // Update BPM line when shown BPM changes
            if (have_shown_bpm && shown_bpm != last_drawn_shown_bpm) {
                last_drawn_shown_bpm = shown_bpm;

                char line0[21];
                std::snprintf(line0, sizeof(line0), "BPM: %3d", shown_bpm);
                (void)lcd.printLine(0, line0);

                // Heart icon at last column (only blinked in NONE)
                (void)lcd.setCursor(19, 0);
                (void)lcd.writeChar((alarm == AlarmType::NONE && heart_on) ? 0 : ' ');
            }

            // If we have no BPM at all, force the BPM line to ---
            if (!have_shown_bpm && last_drawn_shown_bpm != 0) {
                last_drawn_shown_bpm = 0;
                (void)lcd.printLine(0, "BPM: ---");
                (void)lcd.setCursor(19, 0);
                (void)lcd.writeChar(' ');
            }

            // Status messaging
            if (alarm == AlarmType::NO_SIGNAL) {
                // NO_SIGNAL should NOT behave like a critical alarm
                (void)lcd.printLine(1, "NO SIGNAL");
                (void)lcd.printLine(2, "CHECK FINGER/SENSOR");
                (void)lcd.printLine(3, "");
                // Ensure heart is not displayed during NO_SIGNAL
                (void)lcd.setCursor(19, 0);
                (void)lcd.writeChar(' ');
            } else {
                if (!have_shown_bpm) {
                    (void)lcd.printLine(1, "Place finger");
                    (void)lcd.printLine(2, "on sensor...");
                    (void)lcd.printLine(3, "");
                } else {
                    (void)lcd.printLine(1, "Status: OK");
                    (void)lcd.printLine(2, "");
                    (void)lcd.printLine(3, "");
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" bool heart_monitor_peek_bpm(BpmReading* out)
{
    if (!out || !g_bpm_q) return false;
    return (xQueuePeek(g_bpm_q, out, 0) == pdTRUE);
}

extern "C" bool heart_monitor_peek_alarm(AlarmEvent* out)
{
    if (!out || !g_alarm_q) return false;
    return (xQueuePeek(g_alarm_q, out, 0) == pdTRUE);
}
static QueueHandle_t g_bpm_ml_q = nullptr; // stores int (corrected BPM)

extern "C" void heart_monitor_set_bpm_ml(int bpm_ml)
{
    if (!g_bpm_ml_q) return;
    (void)xQueueOverwrite(g_bpm_ml_q, &bpm_ml);
}

extern "C" bool heart_monitor_peek_bpm_ml(int* out)
{
    if (!out || !g_bpm_ml_q) return false;
    return (xQueuePeek(g_bpm_ml_q, out, 0) == pdTRUE);
}


// Public API
extern "C" void heart_monitor_start()
{
    if (!g_bpm_q)   g_bpm_q   = xQueueCreate(1, sizeof(BpmReading));
    if (!g_alarm_q) g_alarm_q = xQueueCreate(1, sizeof(AlarmEvent));
    if (!g_bpm_ml_q) g_bpm_ml_q = xQueueCreate(1, sizeof(int));

    // Seed alarm queue so LCD starts in a known state
    AlarmEvent init_alarm{};
    init_alarm.type = AlarmType::NONE;
    init_alarm.t_ms = now_ms();
    (void)xQueueOverwrite(g_alarm_q, &init_alarm);

    ESP_LOGI(TAG, "Starting heart monitor tasks");

    xTaskCreate(sampler_task,  "hr_sampler", 4096, nullptr, 6, nullptr);
    xTaskCreate(detector_task, "hr_detect",  4096, nullptr, 5, nullptr);
    xTaskCreate(lcd_task,      "hr_lcd",     4096, nullptr, 4, nullptr);

}
