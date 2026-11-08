#include <cctype>
#include <filesystem>
#include <string>

#include "internal.h"
#include "paths.h"

namespace {
bool parse_bool(const std::string& raw, bool* out) {
    std::string v;
    v.reserve(raw.size());
    for (char c : raw) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "yes" || v == "on" || v == "1") {
        *out = true;
        return true;
    }
    if (v == "false" || v == "no" || v == "off" || v == "0") {
        *out = false;
        return true;
    }
    return false;
}
}  // namespace

extern "C" {

phig_status phig_settings_load(const char* path, phig_settings** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    try {
        std::string p = (path && path[0] != '\0') ? std::string(path) : config_path();
        auto* h = new phig_settings();
        h->load_path = p;
        try {
            h->s = Settings::load(p);
        } catch (const std::runtime_error&) {
            if (std::filesystem::exists(p)) {
                // The file exists but Settings::load could not read it (e.g.
                // permissions, not a regular file): report PHIG_ERR_DB rather
                // than treating it as "no config yet".
                delete h;
                return PHIG_ERR_DB;
            }
            // No file at all: hand back a usable empty handle so a caller can
            // populate and save one.
            *out = h;
            return PHIG_ERR_NOT_FOUND;
        }
        *out = h;
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

void phig_settings_free(phig_settings* s) { delete s; }

const char* phig_settings_get(phig_settings* s, const char* key, const char* fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        // Expand ~ on read; cache so the returned pointer stays valid.
        return s->put(s->expanded, Settings::normalize_key(key), expand_tilde(*raw));
    } catch (...) {
        return fallback;
    }
}

const char* phig_settings_get_raw(phig_settings* s, const char* key, const char* fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        return s->put(s->raw_cache, Settings::normalize_key(key), *raw);
    } catch (...) {
        return fallback;
    }
}

int64_t phig_settings_get_int(phig_settings* s, const char* key, int64_t fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        size_t consumed = 0;
        int64_t v = std::stoll(*raw, &consumed);
        // Reject trailing junk: "4x" is not 4.
        if (consumed != raw->size()) return fallback;
        return v;
    } catch (...) {
        return fallback;
    }
}

bool phig_settings_get_bool(phig_settings* s, const char* key, bool fallback) {
    if (s == nullptr || key == nullptr) return fallback;
    try {
        const std::string* raw = s->s.get(key);
        if (raw == nullptr) return fallback;
        bool v = false;
        return parse_bool(*raw, &v) ? v : fallback;
    } catch (...) {
        return fallback;
    }
}

bool phig_settings_has(phig_settings* s, const char* key) {
    if (s == nullptr || key == nullptr) return false;
    try {
        return s->s.has(key);
    } catch (...) {
        return false;
    }
}

size_t phig_settings_count(phig_settings* s) {
    if (s == nullptr) return 0;
    try {
        if (s->key_cache_dirty) {
            s->key_cache = s->s.keys();
            s->key_cache_dirty = false;
        }
        return s->key_cache.size();
    } catch (...) {
        return 0;
    }
}

const char* phig_settings_key_at(phig_settings* s, size_t i) {
    if (s == nullptr) return nullptr;
    try {
        if (s->key_cache_dirty) {
            s->key_cache = s->s.keys();
            s->key_cache_dirty = false;
        }
        if (i >= s->key_cache.size()) return nullptr;
        return s->key_cache[i].c_str();
    } catch (...) {
        return nullptr;
    }
}

phig_status phig_settings_set(phig_settings* s, const char* key, const char* value) {
    if (s == nullptr || key == nullptr || key[0] == '\0' || value == nullptr) {
        return PHIG_ERR_INVALID_ARG;
    }
    try {
        s->s.set(key, value);
        s->refresh_cached(key);
        s->key_cache_dirty = true;
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

phig_status phig_settings_unset(phig_settings* s, const char* key) {
    if (s == nullptr || key == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        s->s.unset(key);
        s->refresh_cached(key);
        s->key_cache_dirty = true;
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

phig_status phig_settings_save(phig_settings* s, const char* path) {
    if (s == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        std::string p = (path && path[0] != '\0') ? std::string(path) : s->load_path;
        namespace fs = std::filesystem;
        fs::path parent = fs::path(p).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            fs::create_directories(parent, ec);
        }
        s->s.save(p);
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_DB;
    }
}

phig_status phig_settings_to_config(phig_settings* s, phig_config* cfg) {
    if (s == nullptr || cfg == nullptr) return PHIG_ERR_INVALID_ARG;
    try {
        struct {
            const char* key;
            const char** field;
        } map[] = {
            {PHIG_KEY_CATALOG, &cfg->catalog_dir},
            {PHIG_KEY_MODELS_DIR, &cfg->models_dir},
        };
        for (auto& m : map) {
            // Only fill fields the caller left unset; never clobber a pre-seed.
            if (*m.field != nullptr) continue;
            const std::string* raw = s->s.get(m.key);
            if (raw == nullptr) continue;
            *m.field = s->put(s->expanded, Settings::normalize_key(m.key), expand_tilde(*raw));
        }
        return PHIG_OK;  // cfg->mode deliberately untouched
    } catch (...) {
        return PHIG_ERR_INTERNAL;
    }
}

}  // extern "C"
