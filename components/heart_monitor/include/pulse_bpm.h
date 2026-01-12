#pragma once
#include <cstdint>

class PulseBpm
{
public:
    enum class Result : int {
        NONE = 0,
        PROVISIONAL = 1,
        STABLE = 2
    };

    void reset(int initial_raw);

    // Returns Result, and fills out_bpm + out_quality if a beat/IBI was accepted.
    Result update(int raw, int64_t t_ms, int& out_bpm, float& out_quality);

    int ibi_count() const { return ibi_count_; }

private:
    static constexpr int IBI_BUF = 5;

    void  update_envelope(float x);
    Result register_beat(int64_t beat_ms, int& out_bpm);

    void push_ibi(int ibi);
    int  average_ibi() const;
    int  median_ibi() const;

private:
    // DC removal + smoothing
    float baseline_ = 0.0f;
    float lp_ = 0.0f;

    // Envelope
    bool  env_inited_ = false;
    float env_min_ = 0.0f;
    float env_max_ = 0.0f;

    // Peak detection state
    int64_t last_beat_ms_ = 0;
    bool    have_prev_ = false;
    float   prev_filt_ = 0.0f;
    int64_t prev_t_ms_ = 0;
    float   diff_prev_ = 0.0f;

    // IBI buffer
    int ibi_buf_[IBI_BUF]{};
    int ibi_count_ = 0;

    // Adaptive stats
    float p2p_ema_   = 0.0f;  // smoothed p2p envelope
    float noise_ema_ = 0.0f;  // smoothed abs slope (noise proxy)

    // for quality/debug
    float last_p2p_ = 0.0f;
};
