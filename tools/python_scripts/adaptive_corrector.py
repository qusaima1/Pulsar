# adaptive_corrector.py
import math

class AdaptiveBpmCorrector:
    """
    Label-free accuracy improvement:
    - quality-weighted smoothing
    - outlier rejection
    - rate limiting
    """

    def __init__(self):
        self.have = False
        self.bpm = 0.0
        self.last_t_ms = 0

    @staticmethod
    def clamp(x, lo, hi):
        return lo if x < lo else hi if x > hi else x

    def reset(self):
        self.have = False
        self.bpm = 0.0
        self.last_t_ms = 0

    def update(self, t_ms: int, bpm_raw: int, q: float, stable: int) -> int:
        # sanitize
        bpm_raw = int(self.clamp(bpm_raw, 30, 220))
        q = float(self.clamp(q, 0.0, 1.0))
        stable = 1 if stable else 0

        if not self.have:
            self.have = True
            self.bpm = float(bpm_raw)
            self.last_t_ms = int(t_ms)
            return int(round(self.bpm))

        dt = int(t_ms) - int(self.last_t_ms)
        if dt <= 0:
            # same timestamp or time went backwards; do not update state
            return int(round(self.bpm))

        self.last_t_ms = int(t_ms)
        dt_s = dt / 1000.0

        # Max physically plausible change rate (bpm per second)
        base_rate = 6.0  # bpm/s
        bonus = 10.0 * q * (1.0 if stable else 0.5)
        max_rate = base_rate + bonus  # ~6..16 bpm/s
        max_step = max_rate * dt_s

        jump = float(bpm_raw) - float(self.bpm)
        abs_jump = abs(jump)

        # outlier threshold depends on quality
        jump_limit = 25.0 if q > 0.6 else 15.0 if q > 0.3 else 8.0

        # extreme spike: ignore
        if abs_jump > 3.0 * jump_limit:
            return int(round(self.bpm))

        # smoothing factor depends on quality
        alpha = 0.08 + 0.55 * q  # 0.08..0.63
        if not stable:
            alpha *= 0.6

        target = self.bpm + alpha * (float(bpm_raw) - self.bpm)

        # rate limit final move
        delta = target - self.bpm
        delta = self.clamp(delta, -max_step, max_step)

        # if big jump, be extra conservative
        if abs_jump > jump_limit:
            delta = self.clamp(delta, -0.5 * max_step, 0.5 * max_step)

        self.bpm += delta
        self.bpm = float(self.clamp(self.bpm, 30.0, 220.0))
        return int(round(self.bpm))
