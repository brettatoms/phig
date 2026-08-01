#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "database.h"
#include "lockfile.h"
#include "phig/phig.h"
#include "settings.h"

struct phig_ctx {
    std::unique_ptr<Database> db;
    phig_access_mode mode = PHIG_READ_ONLY;
    std::string db_path;
    std::string thumbs_dir;
    std::string last_error;
};

struct phig_query {
    Database::PageCriteria criteria;
};

struct phig_settings {
    Settings s;
    std::string load_path;  // for save(NULL)
    // Expanded values handed out by phig_settings_get, kept alive until the
    // key changes or the handle is freed. Keyed by the setting's key.
    std::unordered_map<std::string, std::string> expanded;
    std::vector<std::string> key_cache;  // backs phig_settings_key_at
    // True when key_cache needs rebuilding: on construction, and after any
    // set/unset. Settings::keys() returns by value, so rebuilding key_cache
    // frees the previous vector's backing storage — every pointer previously
    // handed out by phig_settings_key_at would dangle if we rebuilt on every
    // call instead of only when stale.
    bool key_cache_dirty = true;
};

// Builds a phig_image_list (with its own string arena) from search results.
// Defined in records.cpp.
phig_image_list* phig_make_image_list(const std::vector<ImageInfo>& rows);

// Acquires phig's process-wide advisory writer lock for one ABI mutation.
// Non-blocking: a GUI must not freeze behind a long-running CLI job, so a
// contended lock is reported as an error the caller can surface instead.
// Throws std::runtime_error when the lock is held elsewhere. Defined in
// mutate.cpp.
void phig_acquire_write_lock(phig_ctx* ctx, std::unique_ptr<WriterLock>& out);

// Every public entry point wraps its body in these so no exception escapes
// the ABI boundary. Requires a `phig_ctx* ctx` in scope with a valid pointer.
#define PHIG_TRY(ctx)                                  \
    if ((ctx) == nullptr) return PHIG_ERR_INVALID_ARG; \
    (ctx)->last_error.clear();                         \
    try {
#define PHIG_CATCH(ctx)                               \
    }                                                 \
    catch (const std::exception& e) {                 \
        (ctx)->last_error = e.what();                 \
        return PHIG_ERR_DB;                           \
    }                                                 \
    catch (...) {                                     \
        (ctx)->last_error = "unknown internal error"; \
        return PHIG_ERR_INTERNAL;                     \
    }
