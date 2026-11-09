#include "av_log_capture.h"

#include <cstdarg>
#include <mutex>

extern "C" {
#include <libavutil/log.h>
}

namespace {

thread_local std::string* t_sink = nullptr;

void capture_callback(void* avcl, int level, const char* fmt, va_list vl) {
    if (!t_sink || level > AV_LOG_WARNING) return;
    char line[1024];
    int print_prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &print_prefix);
    t_sink->append(line);
}

}  // namespace

void install_av_log_callback() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(capture_callback); });
}

ScopedAvLogCapture::ScopedAvLogCapture(std::string& sink) : previous_(t_sink) {
    install_av_log_callback();
    t_sink = &sink;
}

ScopedAvLogCapture::~ScopedAvLogCapture() { t_sink = previous_; }
