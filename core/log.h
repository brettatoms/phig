#pragma once

#include <cstddef>
#include <string>

enum class LogLevel { Info = 0, Warn = 1, Error = 2 };

// Parse "info" | "warn" | "error"; throws std::runtime_error on anything else.
LogLevel log_level_from_string(const std::string& s);

// Build the process-wide diagnostic logger: a stderr sink (warn and above,
// "[<level>] <msg>") and, when file_enabled, a rotating file sink at `path`
// (UTC ISO-8601 timestamps, embedded newlines escaped so one line is one
// entry, file_level and above, rotating to phig.1.log at max_bytes). If the
// file sink can't be created, warns once on stderr and continues stderr-only.
// Never throws. Call once at startup (tests may call again after log_close()).
void log_init(const std::string& path, bool file_enabled, LogLevel file_level,
              std::size_t max_bytes = std::size_t{5} * 1024 * 1024);

// Log one message to every sink whose level admits it. The message is logged
// verbatim (never treated as a fmt format string — braces in filenames are
// safe). Thread-safe; every entry is flushed. No-op before log_init (or after
// log_close), so library code may call unconditionally.
void log_info(const std::string& msg);
void log_warn(const std::string& msg);
void log_error(const std::string& msg);

// Flush and drop the logger; log_* become no-ops again. For tests.
void log_close();
