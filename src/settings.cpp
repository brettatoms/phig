#include "settings.h"

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
}  // namespace

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
        std::string t = trim(raw);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
            current_section = trim(t.substr(1, t.size() - 2));
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
                std::string name = trim(t.substr(0, eq));
                line.key = current_section.empty() ? name : current_section + "." + name;
                line.value = trim(t.substr(eq + 1));
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

bool Settings::has(const std::string& key) const { return index_.count(key) > 0; }

const std::string* Settings::get(const std::string& key) const {
    auto it = index_.find(key);
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

void Settings::set(const std::string& key, const std::string& value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
        Line& line = lines_[it->second];
        // Rewrite only the value span: keep the original text up to and
        // including '=', and any trailing comment after the old value.
        size_t eq = line.text.find('=');
        std::string head = line.text.substr(0, eq + 1);
        std::string tail = line.text.substr(eq + 1);
        // Find where the old value sits inside `tail` so a trailing comment,
        // and the spacing before it, survive.
        size_t vb = tail.find_first_not_of(" \t");
        std::string lead = (vb == std::string::npos) ? " " : tail.substr(0, vb);
        std::string rest;
        if (vb != std::string::npos) {
            size_t hash = tail.find_first_of("#;", vb);
            if (hash != std::string::npos) rest = tail.substr(hash);
            // Preserve the whitespace that sat between value and comment.
            if (!rest.empty()) {
                size_t ve = tail.find_last_not_of(" \t", hash - 1);
                if (ve != std::string::npos && ve + 1 < hash) {
                    rest = tail.substr(ve + 1);
                }
            }
        }
        line.text = head + lead + value + rest;
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

    // Insert after the last line belonging to the section.
    size_t insert_at = lines_.size();
    bool found_section = section.empty();
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
    auto it = index_.find(key);
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
