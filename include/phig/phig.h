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

typedef enum { PHIG_MEDIA_ANY = 0, PHIG_MEDIA_IMAGE = 1, PHIG_MEDIA_VIDEO = 2 } phig_media_type;

typedef enum {
    PHIG_SORT_DATE = 0,
    PHIG_SORT_PATH = 1,
    PHIG_SORT_SIZE = 2,
    PHIG_SORT_FILENAME = 3
} phig_sort_key;

typedef struct phig_query phig_query;

/* Strings are owned by the containing phig_image_list and are valid until it
 * is freed. Absent values are "" (never NULL). blur_score is -1 if not
 * computed. Dates are ISO-8601 TEXT; effective_date is phig's best available
 * date (EXIF DateTimeOriginal, then DateTimeDigitized, then modified_at) and
 * is what PHIG_SORT_DATE orders by. */
typedef struct {
    int64_t id;
    const char* path;
    const char* filename;
    const char* extension;
    const char* sha256;
    int64_t file_size;
    const char* created_at;
    const char* modified_at;
    const char* effective_date;
    int32_t width;
    int32_t height;
    phig_media_type media_type;
    int64_t duration_ms;
    const char* gallery;
    double blur_score;
    bool favorite;
} phig_image;

typedef struct {
    const phig_image* items;
    int64_t count;
} phig_image_list;

PHIG_API void phig_image_list_free(phig_image_list* list);

PHIG_API phig_status phig_query_new(phig_ctx* ctx, phig_query** out);
PHIG_API void phig_query_free(phig_query* q);

/* Setters ignore NULL query pointers. Passing NULL or "" for a string filter
 * clears that filter. */
PHIG_API void phig_query_set_extension(phig_query* q, const char* ext);
PHIG_API void phig_query_set_date_range(phig_query* q, const char* after, const char* before);
PHIG_API void phig_query_set_camera(phig_query* q, const char* model);
PHIG_API void phig_query_set_make(phig_query* q, const char* make);
PHIG_API void phig_query_set_size_range(phig_query* q, int64_t min_bytes, int64_t max_bytes);
PHIG_API void phig_query_set_media(phig_query* q, phig_media_type media);
PHIG_API void phig_query_set_duration_range(phig_query* q, int64_t min_ms, int64_t max_ms);
PHIG_API void phig_query_add_tag(phig_query* q, const char* tag); /* AND semantics */
PHIG_API void phig_query_set_gallery(phig_query* q, const char* gallery);
PHIG_API void phig_query_set_sharpness(phig_query* q, double min_score, double max_score);
PHIG_API void phig_query_set_sort(phig_query* q, phig_sort_key key, bool descending);

/* Total matching rows, ignoring pagination. Needed to size a scrollbar before
 * any page has been fetched. */
PHIG_API phig_status phig_query_count(phig_ctx* ctx, const phig_query* q, int64_t* out);

/* One page. *out must be released with phig_image_list_free. A page past the
 * end yields an empty list, not an error. limit must be > 0. */
PHIG_API phig_status phig_query_page(phig_ctx* ctx, const phig_query* q, int64_t offset,
                                     int32_t limit, phig_image_list** out);

/* Raw EXIF/container metadata JSON for one image. Deliberately separate from
 * phig_image: it is a large blob and fetching it per grid cell would waste
 * most of the query budget. *json_out must be released with phig_string_free.
 * Returns PHIG_ERR_NOT_FOUND if no such id. */
PHIG_API phig_status phig_image_metadata(phig_ctx* ctx, int64_t id, char** json_out);
PHIG_API void phig_string_free(char* s);

#ifdef __cplusplus
}
#endif
#endif /* PHIG_PHIG_H */
