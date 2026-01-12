import socket
import time
import datetime
import os
from collections import deque

# ESP -> PC telemetry port (you already use this)
IN_PORT = 7777

# PC -> ESP corrected BPM port (ESP will listen on this)
OUT_PORT = 7778

# Log file
ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
log_path = f"heart_ml_log_{ts}.csv"

# --- Simple "ML v0" model parameters ---
# Outlier rejection: allow larger jumps when quality is high? (actually inverse is better)
BASE_MAX_JUMP = 25     # bpm
LOWQ_EXTRA_JUMP = 40   # when quality is low, tolerate less? we use this to adjust threshold
MIN_ALPHA = 0.08       # strongest smoothing (low quality)
MAX_ALPHA = 0.55       # weakest smoothing (high quality)
STABLE_BONUS = 0.10    # if stable, trust measurement more

# window for robust median reference
HIST_N = 7

def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x

def alpha_from_quality(q, stable):
    # q in [0..1]
    a = MIN_ALPHA + (MAX_ALPHA - MIN_ALPHA) * clamp(q, 0.0, 1.0)
    if stable:
        a = clamp(a + STABLE_BONUS, 0.0, 1.0)
    return a

def max_jump_from_quality(q):
    # When quality is low, be stricter about jumps (reject spikes)
    # threshold decreases as q decreases
    # q=1 -> BASE_MAX_JUMP
    # q=0 -> BASE_MAX_JUMP - 15 (stricter)
    return clamp(BASE_MAX_JUMP - (1.0 - clamp(q,0,1)) * 15.0, 10.0, 60.0)

# UDP server socket (ESP -> PC)
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("0.0.0.0", IN_PORT))
print(f"Listening on UDP {IN_PORT}... logging to {os.path.abspath(log_path)}")

# UDP client socket (PC -> ESP)
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# State
hist = deque(maxlen=HIST_N)
bpm_corr = None
last_sender_ip = None

with open(log_path, "w", buffering=1, encoding="utf-8") as f:
    f.write("pc_time_iso,src_ip,t_ms,bpm_raw,quality,stable,alarm_type,bpm_corr\n")

    while True:
        data, (ip, port) = rx.recvfrom(2048)
        last_sender_ip = ip

        line = data.decode(errors="ignore").strip()
        # Expected: t_ms,bpm_raw,quality,stable,alarm_type
        parts = line.split(",")
        if len(parts) != 5:
            print("[WARN] bad packet:", line)
            continue

        t_ms_s, bpm_s, q_s, stable_s, alarm_s = parts

        try:
            t_ms = int(t_ms_s)
            bpm_raw = int(bpm_s)
            q = float(q_s)
            stable = (int(stable_s) != 0)
            alarm_type = int(alarm_s)
        except ValueError:
            print("[WARN] parse error:", line)
            continue

        # Initialize corrected BPM with first valid reading
        if bpm_corr is None:
            bpm_corr = float(bpm_raw)
            hist.append(float(bpm_raw))
        else:
            # Robust reference: median of recent raw BPMs
            sorted_hist = sorted(hist) if hist else [bpm_raw]
            median_raw = sorted_hist[len(sorted_hist)//2]

            # Spike rejection based on jump threshold
            jump_thr = max_jump_from_quality(q)
            if abs(bpm_raw - median_raw) > jump_thr and q < 0.6:
                # treat as artifact; do not fully accept
                bpm_used = median_raw
            else:
                bpm_used = bpm_raw

            # quality-adaptive smoothing
            a = alpha_from_quality(q, stable)
            bpm_corr = (1.0 - a) * bpm_corr + a * float(bpm_used)

            hist.append(float(bpm_raw))

        bpm_corr_i = int(round(bpm_corr))

        # Log
        pc_time = datetime.datetime.now().isoformat(timespec="milliseconds")
        f.write(f"{pc_time},{ip},{t_ms},{bpm_raw},{q:.3f},{1 if stable else 0},{alarm_type},{bpm_corr_i}\n")

        # Send corrected BPM back to ESP
        # Format: t_ms,bpm_corr
        if last_sender_ip:
            out_msg = f"{t_ms},{bpm_corr_i}\n".encode()
            tx.sendto(out_msg, (last_sender_ip, OUT_PORT))

        print(f"{pc_time} raw={bpm_raw} q={q:.2f} stable={int(stable)} -> corr={bpm_corr_i}")
