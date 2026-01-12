#include "pulse_bpm.h"

#include <algorithm>
#include <cmath>
#include <climits>

static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static inline int clampi(int v, int lo, int hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// Conservative physiological limits
static constexpr int BPM_MIN = 40;
static constexpr int BPM_MAX = 180;

static constexpr int IBI_MIN_MS = 60000 / BPM_MAX; // 333 ms
static constexpr int IBI_MAX_MS = 60000 / BPM_MIN; // 1500 ms

void PulseBpm::reset(int initial_raw)
{
    baseline_ = (float)initial_raw;
    lp_ = 0.0f;

    env_inited_ = false;
    env_min_ = 0.0f;
    env_max_ = 0.0f;

    last_beat_ms_ = 0;

    have_prev_ = false;
    prev_filt_ = 0.0f;
    prev_t_ms_ = 0;
    diff_prev_ = 0.0f;

    ibi_count_ = 0;
    for (int &v : ibi_buf_) v = 0;

    p2p_ema_ = 0.0f;
    noise_ema_ = 0.0f;
    last_p2p_ = 0.0f;
}

PulseBpm::Result PulseBpm::update(int raw, int64_t t_ms, int& out_bpm, float& out_quality)
{
    // 1) Baseline removal (DC)
    constexpr float alpha_base = 0.01f;
    baseline_ += alpha_base * ((float)raw - baseline_);
    float ac = (float)raw - baseline_;

    // 2) Smooth (low-pass)
    constexpr float alpha_lp = 0.18f;
    lp_ += alpha_lp * (ac - lp_);
    float filt = lp_;

    // 3) Envelope (p2p)
    update_envelope(filt);
    float p2p = env_max_ - env_min_;
    last_p2p_ = p2p;

    // 4) Noise estimate (abs slope EMA)
    // Use slope of filtered signal as a simple noise/motion proxy.
    // Only valid when we have a previous sample.
    if (have_prev_) {
        float diff = filt - prev_filt_;
        constexpr float alpha_noise = 0.06f;              // ~fast enough to follow motion
        noise_ema_ += alpha_noise * (std::fabs(diff) - noise_ema_);
    }

    // 5) Smooth p2p (amplitude) with EMA
    constexpr float alpha_p2p = 0.04f;                    // slower than noise
    if (p2p_ema_ <= 0.0f) p2p_ema_ = p2p;
    else                  p2p_ema_ += alpha_p2p * (p2p - p2p_ema_);

    // 6) Adaptive minimum p2p gate:
    // Require p2p to be above a noise-related floor to avoid triggering on noise.
    // When noise is low, allow smaller p2p to still lock.
    float p2p_min_adapt = std::max(18.0f, 8.0f * noise_ema_);   // tuneable
    p2p_min_adapt = clampf(p2p_min_adapt, 18.0f, 80.0f);

    // 7) Adaptive threshold:
    // - component from amplitude (p2p_ema_)
    // - component from noise (noise_ema_)
    // - plus a minimum floor to prevent ultra-low thresholds
    static constexpr float THR_MIN = 22.0f;
    float thr_from_amp   = 0.26f * p2p_ema_;               // slightly lower than your 0.30 to lock sooner
    float thr_from_noise = 6.0f * noise_ema_;              // raises threshold during noisy periods
    float thr = std::max(THR_MIN, std::max(thr_from_amp, thr_from_noise));

    // 8) Quality score (0..1):
    // - better when p2p_ema is high
    // - better when noise is low
    // - better when IBI history exists (stability)
    float q_amp   = clampf(p2p_ema_ / 140.0f, 0.0f, 1.0f);
    float q_noise = clampf(1.0f - (noise_ema_ / 25.0f), 0.0f, 1.0f);
    float q_stb   = clampf((float)std::min(ibi_count_, 5) / 5.0f, 0.0f, 1.0f);
    out_quality   = clampf(0.55f * q_amp + 0.30f * q_noise + 0.15f * q_stb, 0.0f, 1.0f);

    // Gate if envelope not ready or amplitude too low
    if (!env_inited_ || p2p_ema_ < p2p_min_adapt) {
        have_prev_ = false;
        return Result::NONE;
    }

    // Need previous sample for slope logic
    if (!have_prev_) {
        prev_filt_ = filt;
        prev_t_ms_ = t_ms;
        have_prev_ = true;
        diff_prev_ = 0.0f;
        return Result::NONE;
    }

    // 9) Peak detection via slope sign change
    float diff = filt - prev_filt_;
    const bool slope_was_up   = (diff_prev_ > 0.0f);
    const bool slope_now_down = (diff <= 0.0f);

    // Refractory: use peak time at prev_t_ms_
    const int64_t since_last = (last_beat_ms_ == 0) ? LLONG_MAX : (prev_t_ms_ - last_beat_ms_);
    const bool refractory_ok = (since_last >= IBI_MIN_MS);

    // Prominence check: must be a meaningful fraction of amplitude
    // Use EMA amplitude to stabilize prominence test.
    float prominence = prev_filt_ - env_min_;
    float prom_req   = 0.50f * p2p_ema_;
    const bool prominent_enough = (prominence > prom_req);

    bool beat = false;
    if (refractory_ok && slope_was_up && slope_now_down) {
        // Peak must be above adaptive threshold and prominent
        if (prev_filt_ > thr && prominent_enough) beat = true;
    }

    // Update prevs
    diff_prev_ = diff;
    prev_filt_ = filt;
    prev_t_ms_ = t_ms;

    if (!beat) return Result::NONE;

    return register_beat(prev_t_ms_, out_bpm);
}

void PulseBpm::update_envelope(float x)
{
    if (!env_inited_) {
        env_min_ = x;
        env_max_ = x;
        env_inited_ = true;
        return;
    }

    // Envelope decay: track peaks but let them decay slowly toward signal
    constexpr float decay = 0.01f;

    if (x < env_min_) env_min_ = x;
    else              env_min_ += decay * (x - env_min_);

    if (x > env_max_) env_max_ = x;
    else              env_max_ += decay * (x - env_max_);
}

PulseBpm::Result PulseBpm::register_beat(int64_t beat_ms, int& out_bpm)
{
    if (last_beat_ms_ != 0) {
        int ibi = (int)(beat_ms - last_beat_ms_);

        if (ibi < IBI_MIN_MS || ibi > IBI_MAX_MS) {
            last_beat_ms_ = beat_ms;
            return Result::NONE;
        }

        // Consistency gate: reject doubles / erratic triggers
        if (ibi_count_ >= 3) {
            int med = median_ibi();
            if (med > 0) {
                float ratio = (float)ibi / (float)med;
                if (ratio < 0.85f || ratio > 1.20f) {
                    last_beat_ms_ = beat_ms;
                    return Result::NONE;
                }
            }
        }

        push_ibi(ibi);

        int avg = average_ibi();
        if (avg > 0) {
            out_bpm = 60000 / avg;
            last_beat_ms_ = beat_ms;

            if (ibi_count_ < 3) return Result::PROVISIONAL;
            return Result::STABLE;
        }
    }

    // First beat arms timing
    last_beat_ms_ = beat_ms;
    return Result::NONE;
}

void PulseBpm::push_ibi(int ibi)
{
    ibi_buf_[ibi_count_ % IBI_BUF] = ibi;
    ++ibi_count_;
}

int PulseBpm::average_ibi() const
{
    int n = std::min(ibi_count_, IBI_BUF);
    if (n <= 0) return 0;
    int sum = 0;
    for (int i = 0; i < n; ++i) sum += ibi_buf_[i];
    return sum / n;
}

int PulseBpm::median_ibi() const
{
    int n = std::min(ibi_count_, IBI_BUF);
    if (n <= 0) return 0;

    int tmp[IBI_BUF];
    for (int i = 0; i < n; ++i) tmp[i] = ibi_buf_[i];
    std::sort(tmp, tmp + n);
    return tmp[n / 2];
}
