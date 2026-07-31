#ifndef PHIG_PHIG_H
#define PHIG_PHIG_H

/* libphig — C ABI for phig image collections.
 *
 * Clients must not read phig's SQLite schema or thumbnail cache layout
 * directly; this header is the contract.
 *
 * Thread safety: a phig_ctx is NOT thread-safe. Use one ctx per thread, or
 * serialize access externally.
 *
 * All dates are ISO-8601 TEXT ("YYYY-MM-DD HH:MM:SS"), never epoch integers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The library is built with hidden visibility by default (see CMakeLists.txt);
 * this marks the public entry points for export. */
#if defined(_WIN32)
#define PHIG_API __declspec(dllexport)
#else
#define PHIG_API __attribute__((visibility("default")))
#endif

#define PHIG_ABI_VERSION_MAJOR 1u
#define PHIG_ABI_VERSION_MINOR 0u

typedef enum {
    PHIG_OK = 0,
    PHIG_ERR_INVALID_ARG = 1,
    PHIG_ERR_NOT_FOUND = 2,
    PHIG_ERR_DB = 3,
    PHIG_ERR_READONLY = 4,
    PHIG_ERR_INTERNAL = 5
} phig_status;

typedef enum { PHIG_READ_ONLY = 0, PHIG_READ_WRITE = 1 } phig_access_mode;

typedef struct phig_ctx phig_ctx;

typedef struct {
    const char* db_path;    /* required */
    const char* thumbs_dir; /* NULL = phig's XDG default */
    phig_access_mode mode;
} phig_config;

/* Returns (major << 16) | minor. Clients should compare the major half
 * against PHIG_ABI_VERSION_MAJOR at startup and refuse to run on mismatch. */
PHIG_API uint32_t phig_abi_version(void);

/* On success *out receives a context the caller must pass to phig_shutdown.
 * On failure *out is set to NULL and the return value says why; there is no
 * context to query for a message in that case.
 *
 * PHIG_READ_ONLY opens the database with SQLITE_OPEN_READONLY: the file must
 * already exist and writes fail. Because phig databases are WAL-mode, SQLite
 * still needs to create a -shm file to service reads, so the directory
 * containing db_path must be writable even in read-only mode. A read-only
 * mount or a sandbox with no write access to that directory fails with
 * PHIG_ERR_DB. */
PHIG_API phig_status phig_init(const phig_config* cfg, phig_ctx** out);

/* Safe to call with NULL. */
PHIG_API void phig_shutdown(phig_ctx* ctx);

/* Message for the most recent failed call on this ctx. Never NULL; returns ""
 * when no error has occurred. Valid until the next call on the same ctx. */
PHIG_API const char* phig_last_error(phig_ctx* ctx);

#ifdef __cplusplus
}
#endif
#endif /* PHIG_PHIG_H */
