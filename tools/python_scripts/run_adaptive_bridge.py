# run_adaptive_bridge.py
import socket
import datetime
import os
from adaptive_corrector import AdaptiveBpmCorrector

# ---- CONFIG ----
IN_PORT = 7777     # ESP -> PC
OUT_PORT = 7778    # PC -> ESP
RX_BIND_IP = "0.0.0.0"
TX_TIMEOUT_SEC = 0.0  # non-blocking send

# If your ESP expects just "bpm_corr\n" instead of "t_ms,bpm_corr\n",
# set this to False.
SEND_WITH_TIMESTAMP = True
# ----------------

def parse_telemetry(line: str):
    """
    Expected from ESP (you said your CSV columns are):
      t_ms,bpm_raw,quality,stable,alarm_type,bpm_corr

    This parser accepts either:
      5 fields: t_ms,bpm_raw,quality,stable,alarm_type
      6 fields: t_ms,bpm_raw,quality,stable,alarm_type,bpm_corr_old
    """
    parts = [p.strip() for p in line.strip().split(",") if p.strip() != ""]
    if len(parts) < 5:
        return None

    try:
        t_ms = int(parts[0])
        bpm_raw = int(parts[1])
        quality = float(parts[2])
        stable = int(parts[3])
        alarm_type = int(parts[4])
        bpm_corr_old = int(parts[5]) if len(parts) >= 6 else None
    except ValueError:
        return None

    return t_ms, bpm_raw, quality, stable, alarm_type, bpm_corr_old

def main():
    # UDP RX
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind((RX_BIND_IP, IN_PORT))

    # UDP TX
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx.settimeout(TX_TIMEOUT_SEC)

    # Logging
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = f"heart_adaptive_{ts}.csv"
    abs_path = os.path.abspath(log_path)

    print(f"Adaptive bridge running.")
    print(f"Listening UDP {IN_PORT} on {RX_BIND_IP}")
    print(f"Sending corrected BPM to UDP {OUT_PORT} (back to sender IP)")
    print(f"Logging -> {abs_path}")
    print()

    corrector = AdaptiveBpmCorrector()

    last_sent_t_ms = None  # ensures "send only on new reading"

    with open(log_path, "w", buffering=1, encoding="utf-8") as f:
        f.write("pc_time_iso,src_ip,t_ms,bpm_raw,quality,stable,alarm_type,bpm_corr\n")

        while True:
            data, (ip, _) = rx.recvfrom(2048)
            line = data.decode(errors="ignore").strip()

            parsed = parse_telemetry(line)
            if not parsed:
                continue

            t_ms, bpm_raw, q, stable, alarm_type, _ = parsed

            # Only process/send when new reading arrives
            if last_sent_t_ms is not None and t_ms == last_sent_t_ms:
                continue

            bpm_corr = corrector.update(t_ms, bpm_raw, q, stable)

            # Log
            pc_time = datetime.datetime.now().isoformat(timespec="milliseconds")
            f.write(f"{pc_time},{ip},{t_ms},{bpm_raw},{q:.3f},{stable},{alarm_type},{bpm_corr}\n")

            # Send back to ESP
            if SEND_WITH_TIMESTAMP:
                out_msg = f"{t_ms},{bpm_corr}\n".encode()
            else:
                out_msg = f"{bpm_corr}\n".encode()

            try:
                tx.sendto(out_msg, (ip, OUT_PORT))
                last_sent_t_ms = t_ms
            except OSError:
                # If bus/network hiccups, just continue (do not crash)
                pass

            print(f"{pc_time}  raw={bpm_raw:3d}  q={q:0.2f}  st={stable}  -> corr={bpm_corr:3d}")

if __name__ == "__main__":
    main()
