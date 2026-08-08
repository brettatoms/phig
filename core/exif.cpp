#include "exif.h"

#include <libexif/exif-data.h>

#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "image_codec.h"

namespace {

// Escape a string for JSON output
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
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

}  // anonymous namespace

std::string extract_exif_json(const std::string& path) {
    // exif_data_new_from_file scans for a JPEG APP1 segment, which a HEIF
    // container does not have — its EXIF lives in a separate item. Pull that
    // block out first and parse it from memory, so the mapping below is shared
    // by both paths and the resulting JSON keys are identical.
    ExifData* data = nullptr;
    if (is_codec_extension(std::filesystem::path(path).extension().string())) {
        std::vector<unsigned char> blob = codec_exif_blob(path);
        if (!blob.empty()) {
            // libexif parses JPEG APP1 payloads, which begin with "Exif\0\0"
            // before the TIFF header. A HEIF Exif item is a bare TIFF block, and
            // handing that over unframed fails *silently*: exif_data_new_from_data
            // returns a non-null ExifData with every IFD empty, so the caller sees
            // "no EXIF" rather than an error. Measured on exif_sample.heic: 0
            // entries unframed, 6 framed.
            static constexpr unsigned char kExifHeader[6] = {'E', 'x', 'i', 'f', 0, 0};
            blob.insert(blob.begin(), std::begin(kExifHeader), std::end(kExifHeader));
            data = exif_data_new_from_data(blob.data(), static_cast<unsigned int>(blob.size()));
        }
    } else {
        data = exif_data_new_from_file(path.c_str());
    }

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
        json << "\"" << json_escape(entries[i].first) << "\":\"" << json_escape(entries[i].second)
             << "\"";
    }
    json << "}";
    return json.str();
}
