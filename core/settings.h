#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// A phig config file (INI: `[section]` headers, `key = value`, `#`/`;`
// comments) held as its original lines plus an index over them, so writing a
// key back rewrites one line and leaves every comment, blank, ordering, and
// unknown key byte-identical.
//
// Keys are "section.name"; a key appearing before any section header is just
// "name". Keys are normalized for identity — lowercased with '-' mapped to
// '_', on both the section and the name — matching config.cpp's normalize()
// and inih, so libphig and the CLI resolve the same file to the same keys. The
// normalization affects lookup and keys() only; the line's original text is
// preserved byte-for-byte on save. On a duplicate key the last one wins,
// matching how a sequential parser would resolve it.
class Settings {
   public:
    // Lowercase and map '-' to '_'. Public so callers can present the same
    // key form the index uses.
    static std::string normalize_key(const std::string& key);

    // Throws std::runtime_error if the file cannot be read. A file that exists
    // but is empty is valid and yields no keys.
    static Settings load(const std::string& path);

    bool has(const std::string& key) const;
    // Raw value as written (no tilde expansion); nullptr when absent.
    const std::string* get(const std::string& key) const;

    // Rewrites the key's existing line in place, or appends it to its section
    // (creating the section at end of file when absent). A dotless key belongs
    // to no section, so a new one is inserted before the first section header
    // — the only position where it parses back as dotless.
    void set(const std::string& key, const std::string& value);
    // Removes the key's line. No-op when absent.
    void unset(const std::string& key);

    // Keys in file order, in normalized form (what get/set expect).
    std::vector<std::string> keys() const;

    // Atomic: writes a temp sibling, then renames over the destination.
    // Throws on failure.
    void save(const std::string& path) const;

    const std::string& source_path() const { return source_path_; }

   private:
    enum class LineKind { Section, KeyValue, Other };
    struct Line {
        LineKind kind = LineKind::Other;
        std::string text;     // the full original line, no newline
        std::string section;  // for Section and KeyValue
        std::string key;      // for KeyValue: the dotted key
        std::string value;    // for KeyValue: the trimmed value, comment stripped
        // for KeyValue: the inline comment, including its leading whitespace
        // and marker (e.g. "  ; note"); empty when there is none. Matches
        // inih's rule: only ';' preceded by whitespace starts an inline
        // comment; '#' never does.
        std::string comment;
    };

    std::vector<Line> lines_;
    std::unordered_map<std::string, size_t> index_;  // key -> index into lines_
    std::string source_path_;
    // Preserve the file's line ending style. A single file-wide flag: a file
    // with mixed CRLF/LF endings normalizes to the dominant style on save.
    bool crlf_ = false;
    bool trailing_newline_ = true;

    void reindex();
};
