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
#define PHIG_ABI_VERSION_MINOR 1u

/* Values are published ABI: append only, never renumber. */
typedef enum {
    PHIG_OK = 0,
    PHIG_ERR_INVALID_ARG = 1,
    PHIG_ERR_NOT_FOUND = 2,
    PHIG_ERR_DB = 3,
    PHIG_ERR_READONLY = 4,
    PHIG_ERR_INTERNAL = 5,
    /* The database's schema version does not match what this build of libphig
       queries. See phig_init. */
    PHIG_ERR_SCHEMA = 6
} phig_status;

typedef enum { PHIG_READ_ONLY = 0, PHIG_READ_WRITE = 1 } phig_access_mode;

typedef struct phig_ctx phig_ctx;

typedef struct {
    const char* db_path; /* required */

    /* Thumbnail cache directory. NULL uses phig's default
       (~/.cache/phig/thumbs, honoring XDG_CACHE_HOME).

       PROCESS-WIDE: phig resolves the cache location through a global, so this
       applies to every context in the process, not just this one. Set it once,
       at startup, before creating any context — the way a host reads its config
       once. Passing different values for two contexts in one process is not
       supported; the last one set wins for all of them. The override is applied
       before the database is opened, so it takes effect even if phig_init
       subsequently fails. */
    const char* thumbs_dir;
    phig_access_mode mode;

    const char* models_dir; /* NULL = phig's default (~/.cache/phig/models) */
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
 * PHIG_ERR_DB.
 *
 * A read-only context cannot run migrations, so phig_init checks the schema
 * version and returns PHIG_ERR_SCHEMA rather than handing back a context whose
 * queries would fail one call later. An older database needs a read-write
 * phig operation (any CLI command, or phig_init with PHIG_READ_WRITE) to
 * migrate it first; a newer one needs a newer libphig. Because a failed init
 * produces no context, there is nothing to call phig_last_error on — the
 * status code is the whole diagnosis.
 *
 * PHIG_READ_WRITE migrates the database up to the version this build expects,
 * and still fails with PHIG_ERR_SCHEMA if the database is newer. */
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

/* Resolves the cache path for an image's thumbnail. Returns a PATH rather than
 * bytes so a client can hand the file straight to its image loader with no
 * copy through this ABI, and so the cache layout stays private to phig.
 *
 * `sha256` must be a 64-char hex digest. On PHIG_OK, out_path holds the path
 * and *exists says whether the file is present; on false the client should
 * request generation rather than reading it. Returns PHIG_ERR_INVALID_ARG if
 * out_len is too small for the path plus its NUL.
 *
 * Resolves against the thumbnail cache directory configured via
 * phig_config.thumbs_dir (process-wide; see that field's documentation), or
 * phig's default if none was set. */
PHIG_API phig_status phig_thumb_lookup(phig_ctx* ctx, const char* sha256, char* out_path,
                                       size_t out_len, bool* exists);

/* Metadata writes. These take ids because a GUI always has a concrete
 * selection; the CLI's glob-based paths are unchanged.
 *
 * All require a PHIG_READ_WRITE context and return PHIG_ERR_READONLY on a
 * read-only one. Each call is one transaction: it applies to every id or to
 * none. Unknown ids are skipped, not an error. n == 0 is a no-op returning
 * PHIG_OK. Favorites are phig's built-in "favorite" tag.
 *
 * If another phig process holds the writer lock (e.g. a running scan), these
 * return PHIG_ERR_DB immediately rather than blocking; phig_last_error explains.
 * A GUI should surface that and retry, never freeze waiting. */
PHIG_API phig_status phig_tag_add(phig_ctx* ctx, const int64_t* ids, size_t n, const char* tag);
PHIG_API phig_status phig_tag_remove(phig_ctx* ctx, const int64_t* ids, size_t n, const char* tag);

/* gallery must be a non-empty string; an empty string returns
 * PHIG_ERR_INVALID_ARG rather than clearing it, so a GUI text field the user
 * blanked out cannot silently clear the gallery. Use phig_gallery_clear to
 * remove an image's gallery. */
PHIG_API phig_status phig_gallery_set(phig_ctx* ctx, const int64_t* ids, size_t n,
                                      const char* gallery);
PHIG_API phig_status phig_gallery_clear(phig_ctx* ctx, const int64_t* ids, size_t n);
PHIG_API phig_status phig_favorite_set(phig_ctx* ctx, const int64_t* ids, size_t n, bool on);

/* ---- Settings -----------------------------------------------------------
 * phig's config file as key/value data. Keys are "section.name", e.g.
 * "paths.db". Access is generic rather than a fixed struct so settings phig
 * adds later are readable without recompiling against a new header.
 *
 * Keys are matched the way phig's own config loader matches them: lowercased,
 * with '-' mapped to '_', on both the section and the name. So "General.Parallel"
 * and "general.parallel" are the same key, and a file written as `[General]`
 * is found by `get("general.parallel")` and rewritten in place. Enumeration
 * returns this normalized form, which is the form get/set expect.
 *
 * These calls take no phig_ctx, so they have no phig_last_error to consult:
 * the status code (or the fallback value) is the whole error report. That is
 * deliberate — a settings handle is usable before any context exists.
 *
 * A phig_settings is NOT thread-safe; use one per thread or serialize access.
 */

typedef struct phig_settings phig_settings;

/* Known keys. Arbitrary string keys work too — these exist for typo
 * resistance on the documented ones. */
#define PHIG_KEY_DB "paths.db"
#define PHIG_KEY_THUMBS_DIR "paths.thumbs_dir"
#define PHIG_KEY_MODELS_DIR "paths.models_dir"
#define PHIG_KEY_PARALLEL "general.parallel"
#define PHIG_KEY_LOG_ENABLED "log.enabled"
#define PHIG_KEY_LOG_MAX "log.max_entries"
#define PHIG_KEY_LOG_FILE "log.file"
#define PHIG_KEY_LOG_LEVEL "log.file_level"

/* Loads `path`, or phig's default config file when path is NULL
 * (~/.config/phig/config.ini, honoring XDG_CONFIG_HOME).
 *
 * Returns PHIG_ERR_NOT_FOUND when no file exists — *out still receives a valid
 * empty handle, so a caller wanting defaults can ignore that status and a
 * caller creating a config can populate and save it. Returns PHIG_ERR_DB when
 * the file exists but cannot be read, with *out set to NULL. */
PHIG_API phig_status phig_settings_load(const char* path, phig_settings** out);

/* Safe to call with NULL. */
PHIG_API void phig_settings_free(phig_settings* s);

/* Returns `fallback` when the key is absent. The returned pointer is owned by
 * `s` and stays valid until `s` is freed. Setting or unsetting the key updates
 * the pointed-to string in place rather than freeing it, and for any value up
 * to PATH_MAX bytes (the realistic case for the paths and settings this API
 * carries) the update lands in the SAME buffer, so the pointer also stays
 * CURRENT. A value longer than PATH_MAX instead moves to a new buffer; the old
 * one is kept alive rather than freed, so the pointer remains safe to read but
 * reports the value as of when it was fetched, not the new one. Path values
 * have a leading `~` expanded, matching the CLI; the unexpanded form is what
 * phig_settings_save writes back. */
PHIG_API const char* phig_settings_get(phig_settings* s, const char* key, const char* fallback);

/* The value exactly as stored in the file, with no `~` expansion. A settings
 * dialog wants this: showing the expanded form and writing it back would
 * hard-code one machine's home directory over the portable `~`. Ownership and
 * lifetime match phig_settings_get. */
PHIG_API const char* phig_settings_get_raw(phig_settings* s, const char* key, const char* fallback);

/* Return `fallback` when the key is absent or its value does not parse as the
 * requested type. Bools accept true/false/yes/no/on/off/1/0, case-insensitive. */
PHIG_API int64_t phig_settings_get_int(phig_settings* s, const char* key, int64_t fallback);
PHIG_API bool phig_settings_get_bool(phig_settings* s, const char* key, bool fallback);

PHIG_API bool phig_settings_has(phig_settings* s, const char* key);

/* Enumeration, for building a settings UI without a compiled-in schema.
 * Indices are stable until the next set/unset. key_at returns NULL when i is
 * out of range. */
PHIG_API size_t phig_settings_count(phig_settings* s);
PHIG_API const char* phig_settings_key_at(phig_settings* s, size_t i);

/* Sets or replaces a key. Keys phig does not know are allowed and preserved. */
PHIG_API phig_status phig_settings_set(phig_settings* s, const char* key, const char* value);
PHIG_API phig_status phig_settings_unset(phig_settings* s, const char* key);

/* Writes to `path`, or the loaded/default path when NULL. Writes a temp
 * sibling and renames over the destination, so a crash mid-save leaves the
 * previous file intact rather than a truncated one. The bytes are not fsynced,
 * so a power loss just after the rename can still lose them. Comments,
 * ordering, and keys phig does not model are preserved. */
PHIG_API phig_status phig_settings_save(phig_settings* s, const char* path);

/* Fills cfg->db_path/thumbs_dir/models_dir from settings, leaving fields the
 * config does not set untouched so a caller can pre-seed defaults. Does NOT
 * set cfg->mode — that is a per-context decision, not a user preference.
 *
 * The strings point into `s`, so `s` must outlive the phig_init call. They
 * survive a later set/unset of the same key — the cached string is never
 * freed — so a load/to_config/edit/save/phig_init sequence is safe. For any
 * new value up to PATH_MAX bytes, the string is rewritten in place, so the
 * pointer also reflects the edit. A value longer than PATH_MAX instead moves
 * to a new buffer, and the pointer keeps reading the pre-edit value rather
 * than dangling or picking up the new one — safe, but stale; re-run
 * phig_settings_to_config if you need the current value in that case. */
PHIG_API phig_status phig_settings_to_config(phig_settings* s, phig_config* cfg);

#ifdef __cplusplus
}
#endif
#endif /* PHIG_PHIG_H */
