#include "exif.h"
#include <libexif/exif-data.h>
#include <sstream>
#include <string>
#include <vector>
#include <utility>
#include <cstring>

namespace {

// Escape a string for JSON output
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void collect_entries(ExifContent* content, void* user_data) {
    auto* entries = static_cast<std::vector<std::pair<std::string, std::string>>*>(user_data);

    for (unsigned int i = 0; i < content->count; i++) {
        ExifEntry* entry = content->entries[i];
        const char* tag_name = exif_tag_get_name_in_ifd(entry->tag, exif_content_get_ifd(content));
        if (!tag_name) continue;

        char value[1024];
        exif_entry_get_value(entry, value, sizeof(value));
        if (value[0] == '\0') continue;

        entries->emplace_back(std::string(tag_name), std::string(value));
    }
}

} // anonymous namespace

std::string extract_exif_json(const std::string& path) {
    ExifData* data = exif_data_new_from_file(path.c_str());
    if (!data) {
        return "{}";
    }

    std::vector<std::pair<std::string, std::string>> entries;
    exif_data_foreach_content(data, collect_entries, &entries);
    exif_data_unref(data);

    if (entries.empty()) {
        return "{}";
    }

    std::ostringstream json;
    json << "{";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) json << ",";
        json << "\"" << json_escape(entries[i].first) << "\":\""
             << json_escape(entries[i].second) << "\"";
    }
    json << "}";
    return json.str();
}
