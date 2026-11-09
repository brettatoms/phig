#pragma once

#include <string>

// Installs FFmpeg's log callback once. While a ScopedAvLogCapture is alive on
// a thread, FFmpeg messages from that thread are appended to its buffer;
// every other message is dropped.
class ScopedAvLogCapture {
   public:
    explicit ScopedAvLogCapture(std::string& sink);
    ~ScopedAvLogCapture();
    ScopedAvLogCapture(const ScopedAvLogCapture&) = delete;
    ScopedAvLogCapture& operator=(const ScopedAvLogCapture&) = delete;

   private:
    std::string* previous_;
};

// Idempotent. The callback replaces FFmpeg's default stderr output, so a
// thread with no active capture prints nothing.
void install_av_log_callback();
