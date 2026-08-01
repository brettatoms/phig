#include "settings.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "paths.h"

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Find the start of an inline comment in `s`, matching inih's
// ini_find_chars_or_comment: only ';' preceded by a whitespace character
// starts an inline comment. '#' is never an inline comment marker (it only
// starts a whole-line comment, handled elsewhere). Returns npos if none.
size_t find_inline_comment(const std::string& s) {
    bool was_space = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (was_space && s[i] == ';') return i;
        was_space = std::isspace(static_cast<unsigned char>(s[i])) != 0;
    }
    return std::string::npos;
}

// Strip a leading UTF-8 BOM, as inih does before parsing the first line. The
// bytes stay in Line::text so the file still round-trips byte-identically;
// only the parsed key drops them.
const char kBom[] = "\xEF\xBB\xBF";
std::string strip_bom(const std::string& s) {
    if (s.compare(0, 3, kBom) == 0) return s.substr(3);
    return s;
}
}  // namespace

std::string Settings::normalize_key(const std::string& key) {
    std::string out = key;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '-') c = '_';
    }
    return out;
}

Settings Settings::load(const std::string& path) {
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Cannot read config file: " + path);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read config file: " + path);

    std::stringstream buf;
    buf << in.rdbuf();
    std::string content = buf.str();

    Settings s;
    s.source_path_ = path;
    s.crlf_ = content.find("\r\n") != std::string::npos;
    s.trailing_newline_ = content.empty() || content.back() == '\n';

    std::string current_section;
    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        bool last = (nl == std::string::npos);
        std::string raw = content.substr(pos, last ? std::string::npos : nl - pos);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        // A trailing newline produces a final empty piece that is not a line.
        if (last && raw.empty()) break;

        Line line;
        line.text = raw;
        // inih strips a UTF-8 BOM from the very start of the file before
        // parsing; do the same so the first key is not "\xEF\xBB\xBFpaths.db".
        std::string t = trim(s.lines_.empty() ? strip_bom(raw) : raw);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            current_section = normalize_key(trim(t.substr(1, t.size() - 2)));
            line.kind = LineKind::Section;
            line.section = current_section;
        } else if (t.empty() || t[0] == '#' || t[0] == ';') {
            line.kind = LineKind::Other;
        } else {
            size_t eq = t.find('=');  // split on the FIRST '='
            if (eq == std::string::npos) {
                line.kind = LineKind::Other;  // not a key/value; preserve as-is
            } else {
                line.kind = LineKind::KeyValue;
                line.section = current_section;
                std::string name = normalize_key(trim(t.substr(0, eq)));
                line.key = current_section.empty() ? name : current_section + "." + name;
                std::string tail = t.substr(eq + 1);
                size_t comment_at = find_inline_comment(tail);
                if (comment_at == std::string::npos) {
                    line.value = trim(tail);
                } else {
                    // Include the whitespace run right before the marker in
                    // the comment, so the value/comment split point matches
                    // where the value actually ends.
                    size_t ws_start = comment_at;
                    while (ws_start > 0 &&
                           std::isspace(static_cast<unsigned char>(tail[ws_start - 1]))) {
                        --ws_start;
                    }
                    line.value = trim(tail.substr(0, ws_start));
                    line.comment = tail.substr(ws_start);
                }
            }
        }
        s.lines_.push_back(std::move(line));

        if (last) break;
        pos = nl + 1;
    }

    s.reindex();
    return s;
}

void Settings::reindex() {
    index_.clear();
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].kind == LineKind::KeyValue) {
            index_[lines_[i].key] = i;  // last duplicate wins
        }
    }
}

bool Settings::has(const std::string& key) const { return index_.count(normalize_key(key)) > 0; }

const std::string* Settings::get(const std::string& key) const {
    auto it = index_.find(normalize_key(key));
    if (it == index_.end()) return nullptr;
    return &lines_[it->second].value;
}

std::vector<std::string> Settings::keys() const {
    std::vector<std::string> out;
    for (const auto& l : lines_) {
        if (l.kind == LineKind::KeyValue) out.push_back(l.key);
    }
    return out;
}

void Settings::set(const std::string& raw_key, const std::string& value) {
    const std::string key = normalize_key(raw_key);
    auto it = index_.find(key);
    if (it != index_.end()) {
        Line& line = lines_[it->second];
        // Rewrite only the value span: keep the original text up to and
        // including '=', and the line's stored inline comment (never
        // re-derived from the new value — the new value may itself contain
        // '#' or ';', which must not be mistaken for a comment marker).
        // Invariant: head, lead, value, and line.comment are disjoint slices
        // of the original line — reconstruction (head + lead + value +
        // comment) must never let one slice's bytes appear in another, or a
        // read-modify-write (set(key, get(key))) would duplicate them on
        // every save.
        size_t eq = line.text.find('=');
        std::string head = line.text.substr(0, eq + 1);
        std::string tail = line.text.substr(eq + 1);
        // Only scan the part of tail before the stored comment for the old
        // value's leading whitespace, so an empty value can never make the
        // scan run into (and re-capture) the comment's own leading
        // whitespace.
        std::string tail_value = tail.substr(0, tail.size() - line.comment.size());
        size_t vb = tail_value.find_first_not_of(" \t");
        std::string lead;
        if (vb != std::string::npos) {
            lead = tail_value.substr(0, vb);
        } else if (line.comment.empty()) {
            // No old value and no comment to separate from (e.g. "k="):
            // synthesize one space for readability.
            lead = " ";
        } else {
            // No old value, but a comment follows immediately after
            // `tail_value` — its own leading whitespace already separates
            // value from marker, so `lead` must be exactly `tail_value`
            // (however little or much whitespace that is) and not add more.
            lead = tail_value;
        }
        line.text = head + lead + value + line.comment;
        line.value = value;
        return;
    }

    // New key. Split "section.name"; a key with no dot has no section.
    size_t dot = key.find('.');
    std::string section = (dot == std::string::npos) ? "" : key.substr(0, dot);
    std::string name = (dot == std::string::npos) ? key : key.substr(dot + 1);

    Line fresh;
    fresh.kind = LineKind::KeyValue;
    fresh.section = section;
    fresh.key = key;
    fresh.value = value;
    fresh.text = name + " = " + value;

    if (section.empty()) {
        // A dotless key belongs to no section. Appending it at end of file
        // would put it under whatever section happens to be last, so it would
        // reload as "<last_section>.name". Before the first section header is
        // the only position where it parses back as dotless.
        size_t insert_at = lines_.size();
        for (size_t i = 0; i < lines_.size(); ++i) {
            if (lines_[i].kind == LineKind::Section) {
                insert_at = i;
                break;
            }
        }
        // A BOM only counts as a BOM on the first line. Inserting ahead of the
        // line that carries it must carry it along, or the BOM becomes stray
        // bytes mid-file and the new first line keeps a phantom prefix.
        if (insert_at == 0 && !lines_.empty() && lines_[0].text.compare(0, 3, kBom) == 0) {
            lines_[0].text.erase(0, 3);
            fresh.text = std::string(kBom) + fresh.text;
        }
        lines_.insert(lines_.begin() + static_cast<long>(insert_at), fresh);
        reindex();
        return;
    }

    // Insert after the last line belonging to the section.
    size_t insert_at = lines_.size();
    bool found_section = false;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (lines_[i].kind == LineKind::Section && lines_[i].section == section) {
            found_section = true;
            insert_at = i + 1;
        } else if (found_section && lines_[i].section == section &&
                   lines_[i].kind == LineKind::KeyValue) {
            insert_at = i + 1;
        }
    }

    if (!found_section) {
        // Create the section at end of file, preceded by a blank line unless
        // the file is empty.
        if (!lines_.empty()) {
            Line blank;
            blank.kind = LineKind::Other;
            lines_.push_back(blank);
        }
        Line hdr;
        hdr.kind = LineKind::Section;
        hdr.section = section;
        hdr.text = "[" + section + "]";
        lines_.push_back(hdr);
        lines_.push_back(fresh);
    } else {
        lines_.insert(lines_.begin() + static_cast<long>(insert_at), fresh);
    }
    reindex();
}

void Settings::unset(const std::string& key) {
    auto it = index_.find(normalize_key(key));
    if (it == index_.end()) return;
    lines_.erase(lines_.begin() + static_cast<long>(it->second));
    reindex();
}

void Settings::save(const std::string& path) const {
    namespace fs = std::filesystem;
    fs::path dest(path);
    fs::path tmp = temp_sibling(dest);

    const std::string eol = crlf_ ? "\r\n" : "\n";
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        out += lines_[i].text;
        if (i + 1 < lines_.size() || trailing_newline_) out += eol;
    }

    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) throw std::runtime_error("Cannot write config: " + tmp.string());
        o << out;
        o.flush();
        if (!o) throw std::runtime_error("Failed writing config: " + tmp.string());
    }

    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("Cannot replace config: " + dest.string());
    }
}
