#include "format.h"

#include <glib.h>

#include <vector>

namespace phig::gtk {

namespace {

std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += " · ";
        out += part;
    }
    return out;
}

}  // namespace

std::string camera_text(const Details& d) {
    if (d.model.empty()) return d.make;
    if (d.make.empty() || d.model.starts_with(d.make)) return d.model;
    return d.make + " " + d.model;
}

std::string exposure_text(const Details& d) {
    return joined({d.exposure_time, d.f_number, d.iso.empty() ? std::string() : "ISO " + d.iso,
                   d.focal_length});
}

std::string size_text(const Details& d) {
    std::string dimensions;
    if (d.width > 0 && d.height > 0)
        dimensions = std::to_string(d.width) + " × " + std::to_string(d.height);
    char* bytes = g_format_size(static_cast<guint64>(d.file_size > 0 ? d.file_size : 0));
    std::string out = joined({dimensions, bytes});
    g_free(bytes);
    return out;
}

std::string sharpness_text(const Details& d) {
    if (d.blur_score < 0) return "Not measured";
    char buf[32];
    g_snprintf(buf, sizeof buf, "%.1f", d.blur_score);
    return buf;
}

std::string progress_text(const IndexProgress& p) {
    if (p.total == 0) return "Finding files…";
    std::string out = std::to_string(p.done) + " of " + std::to_string(p.total) + " files";
    if (p.errors > 0) out += " · " + std::to_string(p.errors) + " failed";
    return out;
}

std::string summary_text(const IndexSummary& s) {
    std::string out = s.completed ? "Scan finished: " : "Scan cancelled: ";
    out += std::to_string(s.processed) + " added or changed, " + std::to_string(s.skipped) +
           " unchanged";
    if (s.errors > 0) out += ", " + std::to_string(s.errors) + " failed";
    return out;
}

std::string write_error_text(const WriteError& e) {
    if (e.busy) return "Nothing was changed: " + e.message + ". Try again when it finishes.";
    return "Nothing was changed: " + e.message;
}

std::string selection_text(uint64_t n) {
    return std::to_string(n) + (n == 1 ? " photo selected" : " photos selected");
}

}  // namespace phig::gtk
