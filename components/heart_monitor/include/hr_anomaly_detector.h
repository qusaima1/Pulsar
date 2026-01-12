#pragma once
#include <cstdint>
#include <cmath>
#include "heart_monitor_types.h"

// BPM-only anomaly flags (not a medical diagnosis).
class HrAnomalyDetector
{
public:
    struct Config {
        int   brady_bpm = 45;
        int   tachy_bpm = 130;

        // how long BPM must stay abnormal before alarming
        int   sustain_ms = 5000;

        // quality gating
        float min_quality_for_bpm = 0.25f;
        int   no_signal_ms = 3000;

        // rapid change detection
        int   rapid_change_delta_bpm = 35;
        int   rapid_change_window_ms = 5000;

        // alarm clear hysteresis
        int   clear_ms = 3000;
    };

    HrAnomalyDetector() : cfg_{} {}
    explicit HrAnomalyDetector(const Config& cfg) : cfg_(cfg) {}

    // Returns true when alarm state changes (new alarm or cleared).
    bool update(const BpmReading& r, AlarmEvent& out_event)
    {
        last_t_ms_ = r.t_ms;

        // Track "no signal" based on quality
        if (r.quality < cfg_.min_quality_for_bpm || r.bpm <= 0) {
            if (no_signal_since_ms_ == 0) no_signal_since_ms_ = r.t_ms;
        } else {
            no_signal_since_ms_ = 0;
        }

        push_hist(r);

        AlarmType candidate = AlarmType::NONE;

        // 1) NO_SIGNAL has priority
        if (no_signal_since_ms_ != 0 &&
            (r.t_ms - no_signal_since_ms_) >= cfg_.no_signal_ms) {
            candidate = AlarmType::NO_SIGNAL;
        } else {
            const bool usable = (r.quality >= cfg_.min_quality_for_bpm) && (r.stable);

            if (usable) {
                // 2) Sustained brady/tachy
                if (r.bpm > 0 && r.bpm < cfg_.brady_bpm) {
                    if (abnormal_since_ms_ == 0 || abnormal_kind_ != AlarmType::BRADYCARDIA) {
                        abnormal_since_ms_ = r.t_ms;
                        abnormal_kind_ = AlarmType::BRADYCARDIA;
                    }
                    if ((r.t_ms - abnormal_since_ms_) >= cfg_.sustain_ms) {
                        candidate = AlarmType::BRADYCARDIA;
                    }
                } else if (r.bpm > cfg_.tachy_bpm) {
                    if (abnormal_since_ms_ == 0 || abnormal_kind_ != AlarmType::TACHYCARDIA) {
                        abnormal_since_ms_ = r.t_ms;
                        abnormal_kind_ = AlarmType::TACHYCARDIA;
                    }
                    if ((r.t_ms - abnormal_since_ms_) >= cfg_.sustain_ms) {
                        candidate = AlarmType::TACHYCARDIA;
                    }
                } else {
                    abnormal_since_ms_ = 0;
                    abnormal_kind_ = AlarmType::NONE;
                }

                // 3) Rapid change (if not already brady/tachy)
                if (candidate == AlarmType::NONE) {
                    if (detect_rapid_change()) {
                        candidate = AlarmType::RAPID_CHANGE;
                    }
                }
            } else {
                abnormal_since_ms_ = 0;
                abnormal_kind_ = AlarmType::NONE;
            }
        }

        // Hysteresis: require stable normal for clear_ms before clearing an active alarm
        if (active_alarm_ != AlarmType::NONE && candidate == AlarmType::NONE) {
            if (clear_since_ms_ == 0) clear_since_ms_ = r.t_ms;
            if ((r.t_ms - clear_since_ms_) < cfg_.clear_ms) {
                candidate = active_alarm_; // hold
            } else {
                clear_since_ms_ = 0;
            }
        } else {
            clear_since_ms_ = 0;
        }

        // State transition?
        if (candidate != active_alarm_) {
            active_alarm_ = candidate;
            out_event.type = active_alarm_;
            out_event.bpm = r.bpm;
            out_event.quality = r.quality;
            out_event.t_ms = r.t_ms;
            return true;
        }

        return false;
    }

    AlarmType active_alarm() const { return active_alarm_; }

private:
    struct Hist {
        int bpm = 0;
        int64_t t_ms = 0;
    };

    void push_hist(const BpmReading& r)
    {
        hist_[hist_wr_] = { r.bpm, r.t_ms };
        hist_wr_ = (hist_wr_ + 1) % HIST_N;
        if (hist_count_ < HIST_N) hist_count_++;
    }

    bool detect_rapid_change() const
    {
        if (hist_count_ < 2) return false;

        const int newest_idx = (hist_wr_ + HIST_N - 1) % HIST_N;
        const auto newest = hist_[newest_idx];

        for (int i = 1; i < hist_count_; ++i) {
            const int idx = (hist_wr_ + HIST_N - 1 - i) % HIST_N;
            const auto old = hist_[idx];

            const int64_t dt = newest.t_ms - old.t_ms;
            if (dt <= 0) continue;
            if (dt > cfg_.rapid_change_window_ms) break;

            const int dbpm = std::abs(newest.bpm - old.bpm);
            if (dbpm >= cfg_.rapid_change_delta_bpm) return true;
        }
        return false;
    }

private:
    Config cfg_;

    AlarmType active_alarm_ = AlarmType::NONE;

    int64_t last_t_ms_ = 0;

    int64_t no_signal_since_ms_ = 0;

    int64_t abnormal_since_ms_ = 0;
    AlarmType abnormal_kind_ = AlarmType::NONE;

    int64_t clear_since_ms_ = 0;

    static constexpr int HIST_N = 8;
    Hist hist_[HIST_N]{};
    int hist_wr_ = 0;
    int hist_count_ = 0;
};
