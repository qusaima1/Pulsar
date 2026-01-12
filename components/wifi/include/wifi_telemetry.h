#pragma once
#include <cstdint>

// Call once from app_main before starting your heart monitor tasks.
// Blocks until Wi-Fi is connected (or times out).
bool wifi_init_sta_blocking();

// Start a task that streams BPM + alarm state to your PC via UDP.
// It reads your existing queues from heart_monitor_tasks.cpp via extern getters.
void telemetry_start();
