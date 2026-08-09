#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "faces_engine.h"
#include "lockfile.h"
#include "organize_engine.h"
#include "paths.h"
#include "phig/phig.h"
#include "progress_queue.h"
#include "scan_engine.h"
#include "settings.h"
#include "thumbs_engine.h"

struct phig_ctx {
    std::unique_ptr<Database> db;
    phig_access_mode mode = PHIG_READ_ONLY;
    std::string db_path;
    std::string thumbs_dir;
};

// The error from the last PHIG_TRY/PHIG_CATCH call on the calling thread.
// Thread-local rather than per-ctx: an error is inherently per-call, and two
// threads sharing one phig_ctx must not see each other's failures. Defined in
// context.cpp.
std::string& phig_thread_last_error();

struct phig_query {
    Database::PageCriteria criteria;
};

struct phig_settings {
    Settings s;
    std::string load_path;  // for save(NULL)
    // A C string handed out to a caller. The bytes live in a heap buffer
    // reserved to at least PATH_MAX, so the `const char*` a caller holds
    // stays valid — and stays current — across any edit whose new value is a
    // realistic path or setting. phig_settings_to_config gives cfg->db_path a
    // pointer in here, and the documented contract only asks that `s`
    // outlive phig_init; it never tells the caller to re-read after an edit.
    // A std::string would defeat this: assigning a longer value moves its
    // buffer.
    struct Cell {
        std::unique_ptr<char[]> buf;
        size_t cap = 0;  // bytes allocated, including the NUL
        // Buffers this cell has outgrown. Kept alive, never written again, so
        // a pointer handed out before the grow is still safe to read — it
        // reports the value as of when it was fetched instead of the newer
        // one. Strictly better than freeing it; growing past the headroom
        // takes a value longer than PATH_MAX, which is not a realistic
        // setting value.
        std::vector<std::unique_ptr<char[]>> retired;

        explicit Cell(const std::string& v) { assign(v); }

        void assign(const std::string& v) {
            if (buf != nullptr && v.size() + 1 <= cap) {
                std::memcpy(buf.get(), v.c_str(), v.size() + 1);
                return;
            }
            // Over-allocate so ordinary edits (a path getting a bit longer)
            // rewrite the existing buffer rather than stranding it. The
            // values this API carries are filesystem paths, so PATH_MAX is
            // the natural floor: it makes every realistic edit reuse the
            // buffer, keeping a held pointer current rather than stale.
            // `cap` only grows (never shrinks), so a cell that has already
            // grown for one long value keeps handling short values too,
            // without shedding the headroom it earned.
            if (buf != nullptr) retired.push_back(std::move(buf));
            size_t want = (v.size() + 1) * 2 + 64;
            cap = std::max<size_t>({cap, want, PATH_MAX});
            buf = std::make_unique<char[]>(cap);
            std::memcpy(buf.get(), v.c_str(), v.size() + 1);
        }
        const char* c_str() const { return buf.get(); }
    };

    // Owns every cell. Cells are never removed while the handle lives: a key
    // that gets unset keeps its cell, holding the empty string.
    std::vector<std::unique_ptr<Cell>> arena;
    // Normalized key -> cell, one per flavor of value. `expanded` backs
    // phig_settings_get and phig_settings_to_config; `raw_cache` backs
    // phig_settings_get_raw.
    std::unordered_map<std::string, Cell*> expanded;
    std::unordered_map<std::string, Cell*> raw_cache;

    std::vector<std::string> key_cache;  // backs phig_settings_key_at
    // True when key_cache needs rebuilding: on construction, and after any
    // set/unset. Settings::keys() returns by value, so rebuilding key_cache
    // frees the previous vector's backing storage — every pointer previously
    // handed out by phig_settings_key_at would dangle if we rebuilt on every
    // call instead of only when stale.
    bool key_cache_dirty = true;

    // Store `value` for `key` in `slot`, reusing that key's existing cell so a
    // pointer already handed out keeps pointing at the current value.
    const char* put(std::unordered_map<std::string, Cell*>& slot, const std::string& key,
                    const std::string& value) {
        auto it = slot.find(key);
        if (it != slot.end()) {
            it->second->assign(value);
            return it->second->c_str();
        }
        arena.push_back(std::make_unique<Cell>(value));
        Cell* c = arena.back().get();
        slot[key] = c;
        return c->c_str();
    }

    // Bring both cached flavors of `key` back in sync after a set/unset, so a
    // pointer a caller is already holding reads the new value rather than a
    // stale or freed one.
    void refresh_cached(const std::string& key) {
        const std::string k = Settings::normalize_key(key);
        auto e = expanded.find(k);
        auto r = raw_cache.find(k);
        if (e == expanded.end() && r == raw_cache.end()) return;
        const std::string* v = s.get(k);
        const std::string raw = (v == nullptr) ? std::string() : *v;
        if (e != expanded.end())
            e->second->assign(v == nullptr ? std::string() : expand_tilde(raw));
        if (r != raw_cache.end()) r->second->assign(raw);
    }
};

// One running long-operation. Defined here rather than in a per-unit header
// because libphig keeps every internal struct in this file; operations.cpp is
// the only translation unit that touches it.
//
// The operation owns its own Database rather than borrowing phig_ctx::db. The
// engines serialize database access behind their own mutex and require that no
// caller touch the connection while run() executes — but the ctx's connection
// stays in use by every query the client keeps making. A separate connection
// keeps those reads safe and lets SQLite arbitrate, which is the whole reason
// these operations need no writer lock.
// A caller-owned organize plan. Independent of the operation that produced it:
// phig_op_organize_plan_take copies it out, so freeing the operation leaves this
// intact. The const char* the accessors hand out point into the actions' own
// std::strings, which is why they stay valid until a set_resolution rewrites a
// destination.
struct phig_organize_plan {
    OrganizePlan plan;
};

struct phig_op {
    enum class Kind { Index, Thumbs, Faces, Plan, Copy, Move };

    explicit phig_op(Kind k) : kind(k) {
        // Reserved up front so the vector never reallocates. Growing it would
        // move the strings, and a short one lives inside its std::string rather
        // than on the heap — the const char* handed out by phig_op_error_at
        // would then dangle, which the header promises it never does.
        retained_errors.reserve(PHIG_OP_MAX_RETAINED_ERRORS);
    }

    Kind kind;
    std::unique_ptr<Database> db;
    CancelToken cancel;

    // Sized to match the CLI's queue. Drop-oldest under back-pressure: a stale
    // position is worthless once a newer one exists.
    ProgressQueue<ScanProgress> index_progress{1024};
    ProgressQueue<ThumbsProgress> thumbs_progress{1024};
    ProgressQueue<FacesProgress> faces_progress{1024};
    ProgressQueue<OrganizeProgress> organize_progress{1024};

    std::thread worker;
    std::atomic<phig_op_state> state{PHIG_OP_RUNNING};

    // Written by the worker before `state` leaves PHIG_OP_RUNNING, read by the
    // caller only after it observes that transition. The release/acquire pair on
    // `state` is what publishes them; nothing else synchronizes these.
    ScanResult index_result;
    ThumbsResult thumbs_result;
    FacesResult faces_result;
    OrganizeResult organize_result;
    std::string failure;

    // Kind::Plan writes its computed plan here for phig_op_organize_plan_take.
    // Kind::Copy/Move read their snapshot from here, taken at _start so the
    // caller may free its own plan the moment the call returns.
    OrganizePlan organize_plan;

    // Per-file failures kept for inspection after the run. Bounded by
    // PHIG_OP_MAX_RETAINED_ERRORS; the result's own error count stays exact.
    // Guarded by `errors_mutex` because two producers append: phig_op_poll on
    // the caller's thread, and the worker sweeping whatever the caller never
    // polled. Both may see the same update, hence `retained_seen`.
    std::mutex errors_mutex;
    std::vector<std::string> retained_errors;
    // Texts already retained, so the two producers cannot record one failure
    // twice. Every error text names its file, so the text identifies the
    // failure. Stops growing once the vector is full, since nothing is added
    // past that point.
    std::unordered_set<std::string> retained_seen;

    // Backing store for the const char* handed out by phig_op_poll, valid until
    // the next poll on this handle — which is exactly what the header promises.
    std::string poll_path;
    std::string poll_error;

    void retain_error(const std::string& text) {
        std::lock_guard<std::mutex> g(errors_mutex);
        if (retained_errors.size() >= PHIG_OP_MAX_RETAINED_ERRORS) return;
        if (!retained_seen.insert(text).second) return;
        retained_errors.push_back(text);
    }
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

// Public entry points that have a `phig_ctx* ctx` in scope wrap their body in
// these so no exception escapes the ABI boundary. The null check on `ctx`
// means entry points with no context (phig_init, records.cpp, settings.cpp)
// can't use this macro and use a plain try/catch(...) instead.
#define PHIG_TRY(ctx)                                  \
    if ((ctx) == nullptr) return PHIG_ERR_INVALID_ARG; \
    phig_thread_last_error().clear();                  \
    try {
#define PHIG_CATCH(ctx)                                      \
    }                                                        \
    catch (const LockBusy& e) {                              \
        phig_thread_last_error() = e.what();                 \
        return PHIG_ERR_BUSY;                                \
    }                                                        \
    catch (const std::exception& e) {                        \
        phig_thread_last_error() = e.what();                 \
        return PHIG_ERR_DB;                                  \
    }                                                        \
    catch (...) {                                            \
        phig_thread_last_error() = "unknown internal error"; \
        return PHIG_ERR_INTERNAL;                            \
    }
