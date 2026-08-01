#pragma once

#include <memory>
#include <string>
#include <vector>

#include "database.h"
#include "lockfile.h"
#include "phig/phig.h"

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
