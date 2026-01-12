// heart_monitor_types.h
#pragma once
#include <cstdint>

enum class AlarmType : uint8_t
{
    NONE = 0,
    NO_SIGNAL = 1,       // low quality / no usable pulse (status, non-critical UI)
    BRADYCARDIA = 2,     // sustained low BPM
    TACHYCARDIA = 3,     // sustained high BPM
    RAPID_CHANGE = 4     // rapid BPM jump / instability
};

static inline const char* alarm_type_str(AlarmType t)
{
    switch (t) {
    case AlarmType::NONE:        return "NONE";
    case AlarmType::NO_SIGNAL:   return "NO_SIGNAL";
    case AlarmType::BRADYCARDIA: return "BRADYCARDIA";
    case AlarmType::TACHYCARDIA: return "TACHYCARDIA";
    case AlarmType::RAPID_CHANGE:return "RAPID_CHANGE";
    default:                     return "UNKNOWN";
    }
}

struct BpmReading
{
    int     bpm = 0;
    float   quality = 0.0f;     // 0..1 signal quality proxy
    bool    stable = false;     // provisional vs stable BPM
    int64_t t_ms = 0;           // timestamp (ms)
};

struct AlarmEvent
{
    AlarmType type = AlarmType::NONE;
    int       bpm = 0;          // contextual snapshot at event time
    float     quality = 0.0f;   // contextual snapshot at event time
    int64_t   t_ms = 0;         // timestamp (ms)
};
