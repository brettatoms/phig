#include "settings.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}
}  // namespace

Settings Settings::load(const std::string& path) {
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
        if (last && raw.empty() && !content.empty()) break;

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

void Settings::set(const std::string&, const std::string&) {
    throw std::logic_error("not implemented");
}
void Settings::unset(const std::string&) { throw std::logic_error("not implemented"); }
void Settings::save(const std::string&) const { throw std::logic_error("not implemented"); }
