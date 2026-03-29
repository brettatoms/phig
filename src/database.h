#pragma once

#include "types.h"
#include "faces.h"
#include <sqlite3.h>
#include <string>
#include <optional>
#include <vector>
#include <array>
#include <functional>

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void init_schema();
    void insert(const ImageInfo& info);
    void insert_batch(const std::vector<ImageInfo>& infos);
    void begin_transaction();
    void commit_transaction();

    // Returns modified_at for a path if it exists in the DB, nullopt otherwise
    std::optional<std::string> get_modified_at(const std::string& path);

    void update_path(const std::string& old_path, const std::string& new_path);

    // Returns all paths in the DB that start with the given prefix
    std::vector<std::string> get_paths_with_prefix(const std::string& prefix);

    // Delete entries by path
    void delete_paths(const std::vector<std::string>& paths);

    // Delete entries matching a path prefix and/or filename glob
    // Returns paths that were deleted
    std::vector<std::string> purge(const std::string& path_prefix, const std::string& glob);

    int64_t count();

    // Get all images, optionally filtered by path prefix
    std::vector<ImageInfo> get_all_images(const std::string& path_prefix = "");

    // Get groups of exact duplicates (same SHA256), optionally filtered by path prefix
    std::vector<std::vector<ImageInfo>> get_exact_duplicates(const std::string& path_prefix = "");

    // Search with multiple criteria
    struct SearchCriteria {
        std::string extension;
        std::string after;           // YYYY-MM-DD
        std::string before;          // YYYY-MM-DD
        std::string camera;          // substring match on EXIF Model
        std::string make;            // substring match on EXIF Make
        int64_t min_size = -1;
        int64_t max_size = -1;
        int limit = -1;
    };
    std::vector<ImageInfo> search(const SearchCriteria& criteria);

    // Face operations
    void insert_face(int64_t image_id, const FaceInfo& face);
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
    void set_face_person(int64_t face_id, int64_t person_id);
    int64_t find_closest_face(const std::array<float, 128>& embedding, float threshold);

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
};
