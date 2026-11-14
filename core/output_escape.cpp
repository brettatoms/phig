#include "output_escape.h"

#include <cstdint>
#include <cstdio>

std::string csv_field(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

namespace {

// Length of the well-formed UTF-8 sequence starting at s[i], or 0 if it is not
// one: a stray continuation byte, a truncated sequence, an overlong encoding, a
// surrogate, or a code point above U+10FFFF.
size_t utf8_sequence_length(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    const uint8_t lead = b(i);
    size_t len = 0;
    uint32_t cp = 0;
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) {
        len = 2;
        cp = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        len = 3;
        cp = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        len = 4;
        cp = lead & 0x07u;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        if ((b(i + k) & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (b(i + k) & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return 0;  // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;                            // surrogate
    if (cp > 0x10FFFF) return 0;
    return len;
}

}  // namespace

std::string json_string(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20) {
            char buf[7];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            const size_t len = utf8_sequence_length(s, i);
            if (len == 0) {
                out += "\xEF\xBF\xBD";  // U+FFFD
                ++i;
                continue;
            }
            out.append(s.substr(i, len));
            i += len;
            continue;
        }
        ++i;
    }
    out += '"';
    return out;
}
