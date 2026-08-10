#pragma once

#include <sqlite3.h>

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "faces.h"
#include "types.h"

// phig's "best available date" for an image: EXIF DateTimeOriginal, then
// DateTimeDigitized, then the file's modified_at. Returns "" if none apply.
// Normalized to one canonical form, "YYYY-MM-DD HH:MM:SS", so every value
// compares lexicographically against every other: EXIF ':' date separators
// become '-', and format_time()'s "YYYY-MM-DDTHH:MM:SSZ" mtime form loses its
// 'T' and 'Z'. Values that match neither shape pass through unchanged.
// Stored in images.effective_date; the v7 migration rewrites older rows into
// this form and its SQL must stay byte-identical to this function.
std::string compute_effective_date(const std::string& metadata_json,
                                   const std::string& modified_at);

class Database {
   public:
    // read_only opens with SQLITE_OPEN_READONLY: the file must already exist
    // (no create-if-missing) and writes fail. Used by libphig's read-only
    // contexts so a GUI can never contend for the writer lock.
    explicit Database(const std::string& path, bool read_only = false);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void init_schema();

    // Schema version recorded in the database (0 = fresh/pre-migration), and
    // the version this build's queries assume. init_schema() reconciles them,
    // but a read-only connection cannot migrate, so libphig compares the two
    // itself and refuses to serve queries against a mismatched file.
    int schema_version();
    static int expected_schema_version();

    void insert(const ImageInfo& info);
    void insert_batch(const std::vector<ImageInfo>& infos);
    void insert_copy(const std::string& source_path, const std::string& dest_path);

    // ---- Activity log (plan 020) ----
    // One operation_entries row: an inverse-able record of a single file change.
    struct OperationEntry {
        std::string kind;  // "move" | "copy" | "purge_row" | "relink" | "move_sidecar" |
                           // "copy_sidecar" | "trash" | "delete_file"
        std::string src;
        std::string dst;           // "" when N/A (purge)
        std::string restore_json;  // "" unless kind == "purge_row"
    };
    // Open an operation row; returns its id. `argv` is the full invocation,
    // `ts` an ISO-8601 UTC timestamp supplied by the caller.
    int64_t log_begin_operation(const std::string& command, const std::string& argv,
                                const std::string& ts);
    // Append one entry to an open operation (bumps entry_count). Both writes run
    // in the CURRENT transaction — the caller owns BEGIN/COMMIT.
    void log_add_entry(int64_t op_id, const OperationEntry& entry);
    // Set the operation's terminal status ("completed" | "interrupted").
    void log_finish_operation(int64_t op_id, const std::string& status);
    // Delete whole operations oldest-first (by id) until the total number of
    // operation_entries rows is <= max_entries. No-op if max_entries <= 0
    // (unlimited) or already under the cap. Runs in its own transaction.
    void log_prune(int max_entries);

    void begin_transaction();
    void commit_transaction();
    void rollback_transaction();

    // Returns modified_at for a path if it exists in the DB, nullopt otherwise
    std::optional<std::string> get_modified_at(const std::string& path);

    // Returns sha256 for a path if it exists in the DB, nullopt otherwise
    std::optional<std::string> get_sha256(const std::string& path);

    void update_path(const std::string& old_path, const std::string& new_path);
    // Transaction-free core of update_path: assumes a transaction is already
    // open (caller owns BEGIN/COMMIT). Throws on failure. Lets a caller commit
    // the path change and a log entry together.
    void update_path_in_txn(const std::string& old_path, const std::string& new_path);

    // Returns all paths in the DB that start with the given prefix
    std::vector<std::string> get_paths_with_prefix(const std::string& prefix);

    // Delete entries by path
    void delete_paths(const std::vector<std::string>& paths);

    // Delete the given paths and, if op_id > 0, log a 'purge_row' entry per
    // deleted path whose restore_json snapshots the row's columns for a future
    // re-insert. All deletes + log writes run in ONE transaction. Returns the
    // number of rows deleted (missing paths are skipped).
    int delete_paths_logged(const std::vector<std::string>& paths, int64_t op_id);

    // Delete entries matching a path prefix and/or filename glob
    // Returns paths that were deleted
    std::vector<std::string> purge(const std::string& path_prefix, const std::string& glob);

    int64_t count();

    // Get all distinct SHA256 hashes in the database
    std::set<std::string> get_all_sha256s();

    // Paths of all rows with this sha256, excluding `exclude_path` (used by
    // scan re-link to find where an externally moved file's content lives now).
    std::vector<std::string> get_paths_for_sha256(const std::string& sha256,
                                                  const std::string& exclude_path);

    // Move user-authored metadata from one image row to another (scan re-link):
    // tags are unioned onto `to_id`; gallery copies only when `to_id`'s is
    // NULL; faces repoint only when `to_id` has none (vec_faces rows are keyed
    // by face_id and are unaffected). Runs in the caller's transaction.
    void transfer_user_metadata(int64_t from_id, int64_t to_id);

    // Get all images, optionally filtered by path prefix
    std::vector<ImageInfo> get_all_images(const std::string& path_prefix = "");

    // Get groups of exact duplicates (same SHA256), optionally filtered by path prefix
    std::vector<std::vector<ImageInfo>> get_exact_duplicates(const std::string& path_prefix = "");

    // Search with multiple criteria
    struct SearchCriteria {
        std::string extension;
        std::string after;   // YYYY-MM-DD
        std::string before;  // YYYY-MM-DD
        std::string camera;  // substring match on EXIF Model
        std::string make;    // substring match on EXIF Make
        int64_t min_size = -1;
        int64_t max_size = -1;
        std::string media;  // "image", "video", or "" (all)
        int64_t min_duration_ms = -1;
        int64_t max_duration_ms = -1;
        std::vector<std::string> tags;  // image must have ALL of these tags
        std::string gallery;            // exact gallery match
        double min_sharpness = -1;      // blur_score >= this
        double max_sharpness = -1;      // blur_score <= this
        int limit = -1;
    };
    std::vector<ImageInfo> search(const SearchCriteria& criteria);

    // Paginated search: same filters as SearchCriteria, plus sort/direction and
    // offset/limit. id is always the tiebreaker so equal sort keys never
    // shuffle rows between pages.
    enum class SortKey { Date, Path, Size, Filename };
    struct PageCriteria {
        SearchCriteria filter;  // reuses the existing filter struct
        SortKey sort = SortKey::Date;
        bool descending = true;
        int64_t offset = 0;
        int32_t limit = 200;
    };
    std::vector<ImageInfo> search_page(const PageCriteria& c);
    int64_t count_matching(const SearchCriteria& filter);

    // Reads images.metadata for one id. Returns false if no such row.
    bool get_metadata_json(int64_t id, std::string& out);

    // Reads path, media_type, and sha256 for one id. Returns false if no such
    // row. Used by phig_thumb_generate, which needs just enough of a row to
    // dispatch generation without a caller-visible full ImageInfo fetch.
    bool get_image_for_thumb(int64_t id, ImageInfo& out);

    // Tags (plan 011). set_tag adds/removes `tag` on the images at the given
    // paths (auto-creating the tag row on add) and returns the number of
    // existing images affected. Favorites use the built-in `favorite` tag.
    int set_tag(const std::vector<std::string>& paths, const std::string& tag, bool add);
    std::vector<std::string> get_tags_for_image(const std::string& path);
    std::vector<std::pair<std::string, int64_t>> get_tag_counts();

    // Id-based sibling of set_tag, for libphig callers that already hold a
    // concrete image id (a GUI selection). Adds or removes `tag` on the given
    // image ids, auto-creating the tag row on add. Unknown ids are skipped.
    // Runs in one transaction. Returns the number of image_tags rows actually
    // inserted/deleted, NOT the number of images touched: on the add path,
    // sqlite3_changes reports 0 for an id/tag pair that already existed
    // (INSERT OR IGNORE hit a duplicate), unlike the unconditional counter in
    // the path-based set_tag.
    int set_tag_by_ids(const std::vector<int64_t>& ids, const std::string& tag, bool add);

    // Galleries (plan 011): at most one per image, stored as a column. set_gallery
    // returns the number of images updated; with no_overwrite it skips images that
    // already have a gallery. clear_gallery returns the number cleared.
    int set_gallery(const std::vector<std::string>& paths, const std::string& gallery,
                    bool no_overwrite);
    int clear_gallery(const std::vector<std::string>& paths);

    // Id-based sibling of set_gallery/clear_gallery. Sets gallery on the given
    // ids; an empty string clears it (stores NULL). Unknown ids are skipped.
    // Runs in one transaction. Returns the number of rows affected.
    int set_gallery_by_ids(const std::vector<int64_t>& ids, const std::string& gallery);
    std::vector<std::pair<std::string, int64_t>> get_gallery_counts();

    // Face operations
    // Inserts a face row and returns its rowid. Returning the id directly means
    // callers need not follow with sqlite3_last_insert_rowid(), which is only
    // correct while no other statement runs on this connection in between.
    int64_t insert_face(int64_t image_id, const FaceInfo& face);
    bool has_faces(const std::string& path);
    void delete_faces_for_image(int64_t image_id);
    int64_t get_image_id(const std::string& path);

    struct FaceSearchResult {
        std::string path;
        float distance;
        int face_x, face_y, face_w, face_h;
    };
    std::vector<FaceSearchResult> search_faces(const std::array<float, 128>& embedding,
                                               float threshold, int limit);

    // People / face naming
    int64_t get_or_create_person(const std::string& name);
    // Returns false when no row matched — the face was deleted since the caller
    // looked it up. Callers reporting a label count must use this, not the
    // number of calls they made.
    bool set_face_person(int64_t face_id, int64_t person_id);
    // Labels a face only if it is still the face the caller compared: still
    // unlabeled, and still carrying `embedding` byte-for-byte. Returns false
    // otherwise, having written nothing.
    //
    // For a caller that read a face id, released its read, and comes back to
    // write. Two clauses carry that. `faces.id` is AUTOINCREMENT as of schema
    // v8, so a deleted id is never issued again and a face deleted in the gap
    // fails `id = ?` and updates nothing. `person_id IS NULL` makes this a
    // compare-and-set against another labeler, which matters because the faces
    // paths run lock-free and two of them can reach the same candidate.
    //
    // `embedding = ?` is belt-and-braces. With ids never reused and no
    // statement that rewrites an embedding in place, it cannot change the
    // outcome of any write phig currently produces. It is kept because it
    // states the contract the caller wants — the write refers to the face that
    // was compared, not to a row number — and would catch an in-place
    // embedding rewrite if one were added.
    bool set_face_person_if_unchanged(int64_t face_id, int64_t person_id,
                                      const std::array<float, 128>& embedding);
    int64_t find_closest_face(const std::array<float, 128>& embedding, float threshold);

    // One face's id and its embedding, read only when the stored blob is
    // exactly 128 floats — a short blob means a corrupt or partially written
    // row, and reading 128 floats from it would run off the end.
    struct FaceEmbeddingRow {
        int64_t face_id;
        std::array<float, 128> embedding;
    };

    // Reference embeddings for a named person, and every face with no person
    // assigned. Rows whose embedding blob is the wrong size are skipped.
    std::vector<FaceEmbeddingRow> get_face_embeddings_for_person(int64_t person_id);
    std::vector<FaceEmbeddingRow> get_unlabeled_face_embeddings();

    struct PersonInfo {
        int64_t id;
        std::string name;
        int64_t face_count;
    };
    std::vector<PersonInfo> list_people();

    // Search by person name
    std::vector<std::string> get_images_for_person(const std::string& name);

    // Auto-label a face if it matches any known person (returns person name or empty)
    std::string auto_label_face(int64_t face_id, const std::array<float, 128>& embedding,
                                float threshold = 0.6);

    // Raw handle for advanced queries
    sqlite3* db_ptr() { return db_; }

   private:
    sqlite3* db_ = nullptr;
    void exec(const std::string& sql);
    int get_schema_version();
    void set_schema_version(int version);

    // Builds the WHERE clause (without the "WHERE" keyword; always starts
    // " AND ...") and appends the values to bind, in order.
    // {value, is_int} matches the existing convention in search().
    void build_filter_sql(const SearchCriteria& c, std::string& sql,
                          std::vector<std::pair<std::string, bool>>& binds);

    // Binds the values from build_filter_sql in order, advancing idx past the
    // last one so the caller can bind its own trailing params (LIMIT/OFFSET).
    void bind_filter_values(sqlite3_stmt* stmt,
                            const std::vector<std::pair<std::string, bool>>& binds, int& idx);

    // Column order must match the SELECT list in search_page().
    ImageInfo row_to_image_info(sqlite3_stmt* stmt);
};
