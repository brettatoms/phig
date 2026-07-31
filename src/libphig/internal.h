#pragma once

#include <memory>
#include <string>

#include "database.h"
#include "phig/phig.h"

struct phig_ctx {
    std::unique_ptr<Database> db;
    phig_access_mode mode = PHIG_READ_ONLY;
    std::string thumbs_dir;
    std::string last_error;
};

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
