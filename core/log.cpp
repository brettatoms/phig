#include "log.h"

#include <spdlog/logger.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum to_spdlog(LogLevel level) {
    switch (level) {
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warn:
            return spdlog::level::warn;
        default:
            return spdlog::level::err;
    }
}

// Custom pattern flag %*: the message with embedded newlines escaped to
// literal \n / \r, so one line in the log file is always exactly one entry.
class EscapedMessageFlag : public spdlog::custom_flag_formatter {
   public:
    void format(const spdlog::details::log_msg& msg, const std::tm&,
                spdlog::memory_buf_t& dest) override {
        for (const char c : std::string_view(msg.payload.data(), msg.payload.size())) {
            if (c == '\n') {
                dest.append(std::string_view("\\n"));
            } else if (c == '\r') {
                dest.append(std::string_view("\\r"));
            } else {
                dest.push_back(c);
            }
        }
    }
    std::unique_ptr<custom_flag_formatter> clone() const override {
        return std::make_unique<EscapedMessageFlag>();
    }
};

}  // namespace

LogLevel log_level_from_string(const std::string& s) {
    if (s == "info") return LogLevel::Info;
    if (s == "warn") return LogLevel::Warn;
    if (s == "error") return LogLevel::Error;
    throw std::runtime_error("invalid log level '" + s + "' (expected info, warn, or error)");
}

void log_init(const std::string& path, bool file_enabled, LogLevel file_level,
              std::size_t max_bytes) {
    std::vector<spdlog::sink_ptr> sinks;

    auto err_sink =
        std::make_shared<spdlog::sinks::stdout_sink_base<spdlog::details::console_mutex>>(stderr);
    err_sink->set_pattern("[%l] %v");
    err_sink->set_level(spdlog::level::warn);
    sinks.push_back(err_sink);

    if (file_enabled) {
        try {
            namespace fs = std::filesystem;
            fs::path p(path);
            if (p.has_parent_path()) {
                fs::create_directories(p.parent_path());
            }
            auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, max_bytes, 1);
            auto formatter =
                std::make_unique<spdlog::pattern_formatter>(spdlog::pattern_time_type::utc);
            formatter->add_flag<EscapedMessageFlag>('*').set_pattern("%Y-%m-%dT%H:%M:%SZ [%l] %*");
            file_sink->set_formatter(std::move(formatter));
            file_sink->set_level(to_spdlog(file_level));
            sinks.push_back(file_sink);
        } catch (const std::exception& e) {
            std::cerr << "[warning] cannot open log file: " << e.what() << "\n";
        }
    }

    g_logger = std::make_shared<spdlog::logger>("phig", sinks.begin(), sinks.end());
    g_logger->set_level(spdlog::level::info);  // per-sink levels do the filtering
    g_logger->flush_on(spdlog::level::info);   // diagnostics are low-volume; flush every entry
}

void log_info(const std::string& msg) {
    if (g_logger) g_logger->info(msg);
}
void log_warn(const std::string& msg) {
    if (g_logger) g_logger->warn(msg);
}
void log_error(const std::string& msg) {
    if (g_logger) g_logger->error(msg);
}

void log_close() {
    if (g_logger) g_logger->flush();
    g_logger.reset();
}
