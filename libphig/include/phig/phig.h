#ifndef PHIG_PHIG_H
#define PHIG_PHIG_H

/* libphig — C ABI for phig image collections.
 *
 * Clients must not read phig's SQLite schema or thumbnail cache layout
 * directly; this header is the contract.
 *
 * Thread safety: a phig_ctx may be called concurrently from multiple threads.
 * The underlying SQLite connection is serialized internally. phig_last_error
 * reports the last error on the calling thread, so it must be read on the
 * same thread whose call failed — not from another thread, and not after a
 * successful call on that thread has cleared it.
 *
 * Not covered by this: the catalog's thumbs/ directory is resolved through a
 * process-wide global (see phig_config.catalog_dir), independent of any
 * phig_ctx.
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

/* Pre-release: this version is deliberately NOT bumped for breaking changes.
 * Nothing outside this repo links libphig — its only consumer is gtk/, built
 * from the same tree — so a bump signals nothing to anybody and the number just
 * drifts. Signatures change in place; callers change with them in the same
 * commit.
 *
 * It still has to match libphig/CMakeLists.txt's VERSION/SOVERSION, which is the
 * trap: leaving a stale libphig.so.N on the library path lets an old
 * implementation load silently. tests/test_libphig_install guards exactly that.
 *
 * Start versioning honestly the first time something outside this repo links
 * against libphig. */
#define PHIG_ABI_VERSION_MAJOR 1u
#define PHIG_ABI_VERSION_MINOR 5u

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
    PHIG_ERR_SCHEMA = 6,
    /* phig's process-wide advisory writer lock is held by another process — a
       running CLI mv/cp/rm/scan/face command, or another client. Returned by
       the metadata writes below, which take the lock without waiting so a GUI
       never freezes behind a long CLI job; nothing was written, and retrying
       once the holder finishes will succeed. The operations at the end of this
       header do not take the lock and never return this. */
    PHIG_ERR_BUSY = 7
} phig_status;

typedef enum { PHIG_READ_ONLY = 0, PHIG_READ_WRITE = 1 } phig_access_mode;

typedef struct phig_ctx phig_ctx;

typedef struct {
    /* The catalog directory: phig.db, its lock, the trash, thumbs/ and
       phig.log live inside it. Required.

       PROCESS-WIDE: phig resolves the catalog's thumbnail and trash
       locations through a global, so the last context initialized decides
       them for every context in the process. Open one catalog per process.
       The catalog is installed before the database is opened, so it takes
       effect even if phig_init then fails.

       PHIG_READ_ONLY requires the catalog to exist at this build's schema.
       PHIG_READ_WRITE creates a missing catalog and migrates an older one.
       Any other value is refused with PHIG_ERR_INVALID_ARG before the catalog
       is touched. */
    const char* catalog_dir;
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
 * containing phig.db must be writable even in read-only mode. A read-only
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

/* Glob selection over the full path, with phig's glob semantics: `*` and `?`
 * are wildcards, a backslash escapes the next character, and brackets are
 * literal. This is NOT SQLite's GLOB, which has [...] character classes and no
 * escape — a pattern means here exactly what it means to the CLI's --match.
 *
 * Repeatable. An image passes when it matches at least one match pattern (or
 * when none was added) and no exclude pattern; an exclude wins where the two
 * overlap. NULL or "" adds nothing rather than matching everything. */
PHIG_API void phig_query_add_match(phig_query* q, const char* glob);
PHIG_API void phig_query_add_exclude(phig_query* q, const char* glob);

/* The ids a query selects. Strings are not involved: this is how a caller names
 * a set of images for an operation that acts on them. */
typedef struct {
    const int64_t* items;
    int64_t count;
} phig_id_list;

/* Resolves a query to every id it selects, in the query's sort order. This
 * resolves a selection rather than paging one, so it ignores pagination
 * entirely — a caller naming a set wants the set, not its first page.
 *
 * *out receives a list the caller must release with phig_id_list_free. A query
 * that matches nothing yields a list with count 0, not an error. */
PHIG_API phig_status phig_query_ids(phig_ctx* ctx, const phig_query* q, phig_id_list** out);
PHIG_API void phig_id_list_free(phig_id_list* list);

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
 * `size` is the thumbnail's longest edge in pixels and is part of its cache
 * path, so sizes are independent: a lookup at 768 does not see a cached 512.
 * Callers should ask for a small set of sizes rather than one per pixel value.
 *
 * Resolves against the catalog's thumbs/ directory (process-wide; see
 * phig_config.catalog_dir). */
PHIG_API phig_status phig_thumb_lookup(phig_ctx* ctx, const char* sha256, int size, char* out_path,
                                       size_t out_len, bool* exists);

/* Generate the thumbnail for one image and write it into phig's cache.
 *
 * `image_id` identifies the row; phig resolves its path, media type, and sha256
 * itself, so a caller needs no knowledge of the cache layout or of how a video's
 * keyframe is chosen. `size` is the longest edge in pixels; generating one size
 * never replaces another.
 *
 * Returns PHIG_OK when a thumbnail now exists in the cache — including when one
 * was already there, so a caller racing another generator does not see a
 * spurious failure. Returns PHIG_ERR_DB if the row does not exist or has no
 * sha256, PHIG_ERR_NOT_FOUND if the row's source file is no longer on disk
 * (moved or deleted after being indexed — an ordinary state, not corruption),
 * and PHIG_ERR_INTERNAL if the image could not be decoded.
 *
 * Decodes pixels, so this is slow relative to every other call in this header:
 * hundreds of milliseconds for a large RAW. Call it off the caller's UI thread.
 * Safe to call concurrently on one ctx; see the thread-safety note above. */
PHIG_API phig_status phig_thumb_generate(phig_ctx* ctx, int64_t image_id, int size);

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
#define PHIG_KEY_CATALOG "paths.catalog"
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

/* Fills cfg->catalog_dir/models_dir from settings, leaving fields the
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

/* ---- Long-running operations ----
 *
 * Three operations: index (scan a directory into the database), thumbs
 * (generate thumbnails), faces (re-detect faces). Separate rather than one
 * fused "scan", because they are independent jobs over the same collection —
 * a client schedules whichever it needs. `phig scan --thumbs --faces` runs all
 * three in one pass; that fusion is a CLI convenience, not the model here.
 *
 * Index is ADDITIVE ONLY, and is not the whole of `phig scan`. It walks, hashes,
 * extracts and upserts; it does not run scan's reconcile phase. Rows whose files
 * have been deleted or moved therefore survive an index run — nothing is
 * removed, no moved file is re-linked to its old row, and no RAW/JPEG pair is
 * demoted. A client must not present it as "sync the library with disk". The
 * reconcile half is deliberately absent until it can be offered as a decision
 * the caller authorizes rather than a silent deletion; see plan 041.
 *
 * Each runs on its own thread and is driven by polling, never a callback. A
 * callback would run library code on the engine's worker thread, potentially
 * while it holds a database lock, and would force every foreign caller (a GTK
 * main loop, an Elixir NIF) to be reentrant on a thread it did not create.
 * Instead: start an operation, poll it from whatever thread suits the caller,
 * read the result when it finishes.
 *
 * Locking: none of these take phig's advisory writer lock. Index's write-set
 * is the whole subtree and unknowable before the walk; thumbs reads the
 * database once and writes only the filesystem cache; faces commits through
 * per-image transactions. SQLite serializes concurrent writers itself (WAL +
 * BEGIN IMMEDIATE + a 30s busy timeout), so a metadata write from this ABI
 * stays available for the whole duration of an operation. PHIG_ERR_BUSY comes
 * from the small mutations above contending with a CLI command that does take
 * the lock (mv/cp/rm), not from these.
 *
 * Thumbnail generation is not a prerequisite for display: phig_thumb_generate
 * fills a miss on demand. Running the thumbs operation warms the cache ahead
 * of a client needing it. The two coexist — a client may keep servicing misses
 * with phig_thumb_generate while the bulk operation runs. The cache is keyed by
 * (sha256, size), so the worst case is that both generate the same thumbnail
 * and write identical bytes to the same path; neither fails, and nothing is
 * corrupted.
 */

typedef struct phig_op phig_op;

typedef enum {
    PHIG_OP_RUNNING = 0,
    /* Finished without being cancelled. A copy or move that had failures still ends
       here; its result's errors, attachment_errors and completed say so. */
    PHIG_OP_DONE = 1,
    /* Ran to completion but stopped early because the caller cancelled. */
    PHIG_OP_CANCELLED = 2,
    /* Aborted. phig_op_error gives the message. */
    PHIG_OP_FAILED = 3
} phig_op_state;

/* What an operation is currently doing. Every operation reports FILE_DONE per
 * item; the others are one-shot notices, and which appear depends on the
 * operation. Values are published ABI: append only, never renumber. */
typedef enum {
    PHIG_STAGE_FILE_DONE = 0,
    /* Index: the directory walk finished; `total` is the file count. */
    PHIG_STAGE_WALKED = 1,
    /* Thumbs/faces: the selection is known; `total` is how many were selected.
       Organize: planning finished; `total` is how many items the plan holds.
       Reused rather than adding a value, since the meaning — "the work is now
       known, here is how much of it there is" — is the same. */
    PHIG_STAGE_SELECTED = 2,
    /* Faces: loading detector models, which is slow and happens before any file
       is processed. `total` is the instance count. */
    PHIG_STAGE_LOADING_FACE_MODELS = 3
} phig_op_stage;

/* A snapshot of an operation's progress, filled by phig_op_poll.
 *
 * `path` and `error` point into the operation and stay valid only until the
 * next phig_op_poll on the same handle. Copy anything to be kept.
 *
 * `error` is non-empty when the file named by `path` failed. The operation
 * continues — a file that cannot be read never aborts a run — and `errors`
 * counts them. A caller polling too slowly to see every update still gets the
 * total from the result, plus a bounded list of retained failures.
 *
 * Every counter is absolute rather than a delta, and every operation fills
 * `done`, `total` and `errors`. `processed` and `skipped` are index and thumbs
 * only — faces has no such split, since a selected file either yields its faces
 * or fails, and leaves both 0. For thumbs, `processed` is the running count of
 * thumbnails generated, matching `generated` on its result. */
typedef struct {
    phig_op_stage stage;
    uint64_t done;  /* items finished, across all outcomes */
    uint64_t total; /* items expected, or the stage-specific count above */
    uint64_t processed;
    uint64_t skipped;
    uint64_t errors;
    const char* path;  /* "" when the stage carries no file */
    const char* error; /* "" unless this file failed */
} phig_op_progress;

/* Results. One per operation rather than a single flattened struct: the three
 * report genuinely different things, and a shared struct would hand a caller
 * fields that are meaningless for the operation it ran. Asking for the wrong
 * one returns PHIG_ERR_INVALID_ARG. */

typedef struct {
    uint64_t total_files; /* files the walk found */
    uint64_t processed;
    uint64_t skipped; /* unchanged since a previous run, or attached to a RAW */
    uint64_t errors;
    uint64_t images;
    uint64_t videos;
    bool completed; /* false if cancellation stopped it early */
} phig_index_result;

typedef struct {
    uint64_t total_selected;
    uint64_t generated;
    uint64_t skipped; /* already cached, or no sha256 to key on */
    uint64_t errors;
    bool completed;
} phig_thumbs_result;

typedef struct {
    uint64_t total_selected;
    uint64_t faces_detected;
    uint64_t errors; /* files that could not be decoded */
    /* Person labels on faces the run did not detect again. A rebuild carries
       each label to the re-detected face whose box overlaps it; these matched
       none and are gone. */
    uint64_t labels_dropped;
    bool completed;
} phig_faces_result;

typedef struct {
    const char* directory; /* required */
    bool recursive;
    bool include_hidden;
    /* Reprocess files a previous scan already indexed, instead of skipping
       those whose path and mtime are unchanged. */
    bool force;
    /* PHIG_MEDIA_ANY indexes both images and videos. A value outside the enum
       is refused with PHIG_ERR_INVALID_ARG. */
    phig_media_type media;
    /* Threads for the parallel hash/extract phase. 0 or negative = auto;
       above 256 is refused. */
    int parallel;
} phig_index_options;

typedef struct {
    /* The images to act on, as ids — resolve a set with phig_query_ids.
       NULL/0 selects NOTHING, not everything: a caller names its subject, and
       an unset selection is a no-op rather than a whole-collection run. The
       array need only outlive the _start call. */
    const int64_t* ids;
    size_t id_count;
    /* Longest edge in pixels. 0 uses phig's default; a negative size is
       refused with PHIG_ERR_INVALID_ARG. Sizes are independent — generating
       one never invalidates another. */
    int size;
    /* Regenerate thumbnails that already exist in the cache. */
    bool force;
    /* 0 or negative = auto; above 256 is refused. */
    int parallel;
} phig_thumbs_options;

typedef struct {
    /* The images to act on, as ids — resolve a set with phig_query_ids.
       NULL/0 selects NOTHING, not everything. That matters more here than on
       any other operation: a rebuild replaces every selected image's face rows,
       carrying person labels to re-detected faces by box overlap and dropping
       (counted in phig_faces_result.labels_dropped) those it cannot match, so
       an unset selection must not sweep the collection. The array need only outlive the _start
       call. */
    const int64_t* ids;
    size_t id_count;
    /* Each worker loads its own detector (~150MB of model state, and the
       detector is not thread-safe), so this bounds memory as much as speed.
       0 or negative = auto, which is deliberately conservative. Above the
       machine's core count is refused: workers are CPU-bound, so one past it
       costs memory and buys nothing. */
    int parallel;
} phig_faces_options;

/* Start an operation. On PHIG_OK, *out receives a handle the caller must pass
 * to phig_op_free — including after it fails or is cancelled. On failure *out
 * is NULL.
 *
 * Each requires a PHIG_READ_WRITE context and returns PHIG_ERR_READONLY on a
 * read-only one. Work begins immediately on an internal thread; these return
 * as soon as it is running.
 *
 * An option value outside its documented set, or an id_count with a NULL ids
 * array, is refused with PHIG_ERR_INVALID_ARG before any work starts, and
 * phig_last_error names the field.
 *
 * The handle does not borrow the context: an operation reads its configuration
 * at start and opens its own database connection, so it keeps running correctly
 * after phig_shutdown. The handle must still be freed. */
PHIG_API phig_status phig_op_index_start(phig_ctx* ctx, const phig_index_options* opts,
                                         phig_op** out);
PHIG_API phig_status phig_op_thumbs_start(phig_ctx* ctx, const phig_thumbs_options* opts,
                                          phig_op** out);
PHIG_API phig_status phig_op_faces_start(phig_ctx* ctx, const phig_faces_options* opts,
                                         phig_op** out);

/* Threading: phig_op_status, phig_op_cancel and phig_op_error may be called
 * from any thread at any time. phig_op_poll, phig_op_wait and phig_op_free must
 * not be called concurrently with themselves or each other — one thread drives
 * a handle's lifecycle. Two threads polling would split the update stream
 * between them; two calling phig_op_wait is undefined. */

/* Current state. NULL reports PHIG_OP_FAILED. */
PHIG_API phig_op_state phig_op_status(phig_op* op);

/* Take the next progress update, if one is waiting.
 *
 * Returns true and fills `out` when an update was available, false when none
 * was — false means "nothing new yet", not "finished"; use phig_op_status for
 * that. A caller typically drains in a loop until false, then repaints.
 *
 * Updates are queued with a bounded capacity and the OLDEST is dropped when a
 * caller cannot keep up, because a stale position is worthless once a newer one
 * exists. Counters in each update are absolute, so dropping intermediate
 * updates loses detail but never leaves a total wrong. */
PHIG_API bool phig_op_poll(phig_op* op, phig_op_progress* out);

/* Ask the operation to stop. Returns immediately; the operation does not stop
 * instantly. Work already in flight finishes first — for faces that is a full
 * detection pass per busy worker — so a caller should show "stopping" rather
 * than treating the operation as over. Poll phig_op_status until it leaves
 * PHIG_OP_RUNNING.
 *
 * Whatever completed before the cancellation is committed, not rolled back: a
 * cancelled index leaves the files it already indexed in the database. The
 * result reports completed = false. Safe to call more than once, and on an
 * operation that has already finished (a no-op). */
PHIG_API void phig_op_cancel(phig_op* op);

/* Block until the operation is no longer running. Equivalent to polling
 * phig_op_status without the loop. Returns once it is DONE, CANCELLED, or
 * FAILED. */
PHIG_API void phig_op_wait(phig_op* op);

/* Read the result. Returns PHIG_ERR_INVALID_ARG if the operation is still
 * running, or if the getter does not match the operation's kind. On a FAILED
 * operation returns PHIG_ERR_INTERNAL — see phig_op_error. */
PHIG_API phig_status phig_op_index_result(phig_op* op, phig_index_result* out);
PHIG_API phig_status phig_op_thumbs_result(phig_op* op, phig_thumbs_result* out);
PHIG_API phig_status phig_op_faces_result(phig_op* op, phig_faces_result* out);

/* Per-file failures retained for inspection after the operation finishes, so a
 * caller that was not polling still learns what went wrong. Retained as the
 * caller polls and again from whatever it never polled, so both a client that
 * watched the run and one that only waited end up with a list; a failure seen
 * by both routes is recorded once. Bounded: at most PHIG_OP_MAX_RETAINED_ERRORS
 * are kept, while the result's `errors` field counts every one. A client that
 * polls slowly enough for updates to be dropped can still lose error text this
 * way, so the count is the authority on how many failed, not the list's length.
 * Index i is in [0, phig_op_error_count).
 *
 * Read them once the operation has stopped: mid-run the list is still growing.
 * Returned pointers stay valid until phig_op_free. */
#define PHIG_OP_MAX_RETAINED_ERRORS 64
PHIG_API size_t phig_op_error_count(phig_op* op);
PHIG_API const char* phig_op_error_at(phig_op* op, size_t i);

/* Why the operation failed as a whole, or "" if it did not. Distinct from the
 * per-file errors above: this is the exception that stopped everything. Reads
 * "" while the operation is still running, since nothing has failed yet.
 * Valid until phig_op_free. */
PHIG_API const char* phig_op_error(phig_op* op);

/* Release the handle. Blocks until the operation's thread has stopped, so a
 * caller that frees a running operation waits for it — call phig_op_cancel
 * first to avoid waiting for a full run. Safe with NULL. After this the handle
 * and every pointer obtained from it are invalid. */
PHIG_API void phig_op_free(phig_op* op);

/* ---- Organize: copy and move (plan 042) -------------------------------- */

/* How one file's destination collision is resolved. */
typedef enum {
    PHIG_CONFLICT_SKIP = 0,
    PHIG_CONFLICT_OVERWRITE = 1,
    PHIG_CONFLICT_RENAME = 2
} phig_conflict_resolution;

typedef struct {
    /* Root of the organized layout. "" is refused. */
    const char* destination;
    /* e.g. "%Y/%m/%original". NULL uses "%Y/%m/%original". Tokens: %original,
       %camera, %make, %gallery, %fav, %Y, %m, %d. Refused: "", an absolute
       path, a ".." component, and an unknown %-token. %camera, %make and
       %gallery each expand to one folder name, so a file's metadata cannot
       place it outside the destination. */
    const char* format;
    /* The images to act on, as ids — resolve a set with phig_query_ids.
       NULL/0 selects NOTHING, not everything: a caller names its subject, and
       an unset selection is a no-op rather than a whole-collection move. The
       array need only outlive the _start call. */
    const int64_t* ids;
    size_t id_count;
    /* Run-level default. A value outside the enum is refused. */
    phig_conflict_resolution on_conflict;
} phig_organize_options;

typedef struct {
    uint64_t applied;           /* parents moved or copied */
    uint64_t skipped;           /* conflicts left alone, or destinations occupied since planning */
    uint64_t errors;            /* parents that failed */
    uint64_t attachment_errors; /* attachments not carried; their parents count in applied */
    uint64_t files_written;     /* including sidecars and attached JPEGs */
    bool cancelled;             /* stopped by the caller partway */
    bool completed;             /* false when cancelled, or when anything failed */
    uint64_t identical;         /* already at the destination with the same bytes; nothing
                                   written, and on a move the source went to the trash */
} phig_organize_result;

/* A plan the caller owns: what would happen, before anything happens.
 *
 * Independent of the operation that computed it — take it, free the operation,
 * and the plan is still valid. Editable, so a caller can answer an individual
 * conflict before authorizing the run. */
typedef struct phig_organize_plan phig_organize_plan;

/* Compute a plan. Writes nothing: no file is moved and no row changes. */
PHIG_API phig_status phig_op_organize_plan_start(phig_ctx* ctx, const phig_organize_options* opts,
                                                 phig_op** out);

/* Take the plan out of a finished plan operation into a caller-owned object.
 * The operation may be freed immediately afterwards. Returns PHIG_ERR_INVALID_ARG
 * if the operation is not a plan operation or has not finished. */
PHIG_API phig_status phig_op_organize_plan_take(phig_op* op, phig_organize_plan** out);

/* Release a plan. Safe with NULL. Every const char* obtained from it becomes
 * invalid. */
PHIG_API void phig_organize_plan_free(phig_organize_plan* plan);

/* Items in the plan, one per photo. Sidecars and attached JPEGs travel with
 * their parent and are not listed separately. */
PHIG_API size_t phig_organize_plan_count(const phig_organize_plan* plan);

/* Strings stay valid until the next phig_organize_plan_set_resolution on this
 * plan, or phig_organize_plan_free. Out-of-range i returns "". */
PHIG_API const char* phig_organize_plan_source_at(const phig_organize_plan* plan, size_t i);
/* The resolved destination, or "" when this item will be skipped. */
PHIG_API const char* phig_organize_plan_dest_at(const phig_organize_plan* plan, size_t i);

/* Whether this item's destination is already taken — by a file on disk, or by
 * an earlier item in the same plan. */
PHIG_API bool phig_organize_plan_conflicts_at(const phig_organize_plan* plan, size_t i);
PHIG_API phig_conflict_resolution phig_organize_plan_resolution_at(const phig_organize_plan* plan,
                                                                   size_t i);

/* Whether the file occupying this item's destination is tracked by phig.
 * Meaningless unless conflicts_at(i). False means phig cannot restore it from
 * its own records, which is when a client should warn hardest. */
PHIG_API bool phig_organize_plan_occupant_tracked_at(const phig_organize_plan* plan, size_t i);

/* Whether this item's destination already holds the same bytes — on disk, or
 * written by an earlier item in the plan — and the two can merge without losing
 * an attachment or user metadata. Not a conflict: dest_at is the intended
 * destination whatever the policy. A copy writes nothing for it; a move sends
 * the source to phig's trash after carrying its tags, favorite, gallery and
 * faces to the destination's row. Both re-check the bytes when they run, and
 * skip the item if they no longer match. */
PHIG_API bool phig_organize_plan_identical_at(const phig_organize_plan* plan, size_t i);

/* Answer one item's conflict, recomputing its destination so dest_at reflects
 * the choice. PHIG_CONFLICT_OVERWRITE carries the caller's authority to replace
 * a file that exists on disk, which the run-level policy does not.
 *
 * Returns PHIG_ERR_INVALID_ARG, changing nothing, when resolution is outside
 * the enum, i is out of range, the item does not conflict, or the destination
 * is another image's not-yet-moved source file — a tracked original phig is in
 * the middle of relocating, which no caller intent may overwrite. */
PHIG_API phig_status phig_organize_plan_set_resolution(phig_organize_plan* plan, size_t i,
                                                       phig_conflict_resolution resolution);

/* Execute a plan. Separate symbols rather than one flag: copy is additive to the
 * database while move rewrites the collection's layout, and a client must not
 * move a library by passing the wrong boolean.
 *
 * Both snapshot the plan at start, so the caller may free it as soon as the call
 * returns. Each destination is re-checked at execution time: one occupied since
 * planning is skipped rather than overwritten, unless set_resolution authorized
 * replacing it. */
PHIG_API phig_status phig_op_copy_start(phig_ctx* ctx, const phig_organize_plan* plan,
                                        phig_op** out);
PHIG_API phig_status phig_op_move_start(phig_ctx* ctx, const phig_organize_plan* plan,
                                        phig_op** out);
PHIG_API phig_status phig_op_organize_result(phig_op* op, phig_organize_result* out);

#ifdef __cplusplus
}
#endif
#endif /* PHIG_PHIG_H */
