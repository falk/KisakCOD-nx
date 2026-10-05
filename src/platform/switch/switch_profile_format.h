#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdint>

// Versioned profiling records are complete, bounded JSON lines. Overflow is
// an explicit event; a truncated record must never look like a valid sample.
namespace kisakperf {
class Json {
    char buffer[1000] = "KPERF ";
    size_t used = 6;
    bool valid = true;
public:
    void Raw(const char *format, ...) {
        if (!valid) return;
        va_list args; va_start(args, format);
        const int n = std::vsnprintf(buffer + used, sizeof(buffer) - used, format, args);
        va_end(args);
        if (n < 0 || (size_t)n >= sizeof(buffer) - used) { valid = false; return; }
        used += (size_t)n;
    }
    void String(const char *value) {
        Raw("\"");
        for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; ++p) {
            if (*p == '"' || *p == '\\') Raw("\\%c", *p);
            else if (*p < 32) Raw("\\u%04x", *p);
            else Raw("%c", *p);
        }
        Raw("\"");
    }
    const char *Line() const {
        return valid ? buffer : "KPERF {\"v\":1,\"type\":\"event\",\"stream\":\"format\",\"clock\":\"source_line\",\"name\":\"overflow\",\"count\":1}";
    }
};
inline void Window(Json &j, const char *stream, uint64_t seq, uint64_t start, uint64_t end,
                   uint64_t frames, uint64_t first, uint64_t last, const char *alignment) {
    j.Raw("{\"v\":1,\"type\":\"metrics\",\"stream\":"); j.String(stream);
    j.Raw(",\"seq\":%llu,\"clock\":\"monotonic_us\",\"time\":%llu,\"unit\":\"us\",\"aggregation\":\"mean_per_frame\",",
          (unsigned long long)seq, (unsigned long long)end);
    j.Raw("\"window\":{\"start_us\":%llu,\"end_us\":%llu,\"samples\":%llu,\"first_frame\":%llu,\"last_frame\":%llu},",
          (unsigned long long)start, (unsigned long long)end, (unsigned long long)frames,
          (unsigned long long)first, (unsigned long long)last);
    j.Raw("\"tags\":{\"timing\":\"elapsed_inclusive\",\"alignment\":"); j.String(alignment);
    j.Raw("},\"metrics\":{");
}
inline void Samples(Json &j, const char *stream, const char *metric, uint64_t seq, const char *fields) {
    j.Raw("{\"v\":1,\"type\":\"samples\",\"stream\":"); j.String(stream);
    j.Raw(",\"seq\":%llu,\"clock\":\"monotonic_us\",\"unit\":\"us\",\"aggregation\":\"sample\",\"metric\":",
          (unsigned long long)seq); j.String(metric);
    j.Raw(",\"fields\":[%s],\"samples\":[", fields);
}
}
