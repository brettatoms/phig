#include "database.h"
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <cmath>

#ifndef SQLITE_VEC_STATIC
#define SQLITE_VEC_STATIC
#endif
#ifndef SQLITE_CORE
#define SQLITE_CORE
#endif
#include "sqlite-vec.h"

namespace fs = std::filesystem;

Database::Database(const std::string& path) {
    // Ensure parent directory exists
    fs::path db_path(path);
    if (db_path.has_parent_path()) {
        fs::create_directories(db_path.parent_path());
    }

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        throw std::runtime_error("Failed to open database: " + err);
    }

    // Enable WAL mode for better concurrent read/write performance
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");

    // Initialize sqlite-vec extension
    sqlite3_vec_init(db_, nullptr, nullptr);
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::init_schema() {
    exec(R"(
        CREATE TABLE IF NOT EXISTS images (
            id          INTEGER PRIMARY KEY,
            path        TEXT NOT NULL UNIQUE,
            filename    TEXT NOT NULL,
            extension   TEXT NOT NULL,
            file_size   INTEGER NOT NULL,
            created_at  TEXT,
            modified_at TEXT,
            sha256      TEXT NOT NULL,
            phash       INTEGER NOT NULL,
            width       INTEGER,
            height      INTEGER,
            exif        TEXT
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_phash ON images(phash)");
    exec("CREATE INDEX IF NOT EXISTS idx_sha256 ON images(sha256)");
    exec("CREATE INDEX IF NOT EXISTS idx_path ON images(path)");

    // Face recognition tables
    exec(R"(
        CREATE TABLE IF NOT EXISTS faces (
            id          INTEGER PRIMARY KEY,
            image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
            embedding   BLOB NOT NULL,
            x           INTEGER,
            y           INTEGER,
            width       INTEGER,
            height      INTEGER
        )
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_faces_image ON faces(image_id)");

    // People table for named faces
    exec(R"(
        CREATE TABLE IF NOT EXISTS people (
            id   INTEGER PRIMARY KEY,
            name TEXT NOT NULL UNIQUE
        )
    )");

    // Add person_id to faces if not exists
    // SQLite doesn't have IF NOT EXISTS for ALTER TABLE, so check first
    {
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, "SELECT person_id FROM faces LIMIT 0", -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            exec("ALTER TABLE faces ADD COLUMN person_id INTEGER REFERENCES people(id)");
        } else {
            sqlite3_finalize(stmt);
        }
    }

    // sqlite-vec virtual table for fast KNN
    exec(R"(
        CREATE VIRTUAL TABLE IF NOT EXISTS vec_faces USING vec0(
            face_id INTEGER PRIMARY KEY,
            embedding float[128]
        )
    )");
}

void Database::insert(const ImageInfo& info) {
    const char* sql = R"(
        INSERT OR REPLACE INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, info.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, info.filename.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, info.extension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, info.file_size);
    sqlite3_bind_text(stmt, 5, info.created_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, info.modified_at.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, info.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, static_cast<int64_t>(info.phash));
    sqlite3_bind_int(stmt, 9, info.width);
    sqlite3_bind_int(stmt, 10, info.height);
    sqlite3_bind_text(stmt, 11, info.exif_json.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert: " + std::string(sqlite3_errmsg(db_)));
    }
}

void Database::insert_batch(const std::vector<ImageInfo>& infos) {
    exec("BEGIN TRANSACTION");
    for (const auto& info : infos) {
        insert(info);
    }
    exec("COMMIT");
}

void Database::insert_copy(const std::string& source_path, const std::string& dest_path) {
    const char* sql = R"(
        INSERT OR REPLACE INTO images
            (path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif)
        SELECT ?, ?, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif
        FROM images WHERE path = ?
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert_copy: " + std::string(sqlite3_errmsg(db_)));
    }

    fs::path dp(dest_path);
    sqlite3_bind_text(stmt, 1, dest_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, dp.filename().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert copy: " + std::string(sqlite3_errmsg(db_)));
    }
}

void Database::begin_transaction() {
    exec("BEGIN TRANSACTION");
}

void Database::commit_transaction() {
    exec("COMMIT");
}

std::optional<std::string> Database::get_modified_at(const std::string& path) {
    const char* sql = "SELECT modified_at FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    std::optional<std::string> result;
    if (rc == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            result = std::string(text);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

void Database::update_path(const std::string& old_path, const std::string& new_path) {
    const char* sql = "UPDATE images SET path = ?, filename = ? WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare update: " + std::string(sqlite3_errmsg(db_)));
    }

    fs::path p(new_path);
    sqlite3_bind_text(stmt, 1, new_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, p.filename().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, old_path.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to update path: " + std::string(sqlite3_errmsg(db_)));
    }
}

std::vector<std::string> Database::get_paths_with_prefix(const std::string& prefix) {
    const char* sql = "SELECT path FROM images WHERE path LIKE ? || '%'";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, prefix.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::string> paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) {
            paths.emplace_back(text);
        }
    }

    sqlite3_finalize(stmt);
    return paths;
}

void Database::delete_paths(const std::vector<std::string>& paths) {
    if (paths.empty()) return;

    exec("BEGIN TRANSACTION");
    const char* sql = "DELETE FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare delete: " + std::string(sqlite3_errmsg(db_)));
    }

    for (const auto& path : paths) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }

    sqlite3_finalize(stmt);
    exec("COMMIT");
}

std::vector<std::string> Database::purge(const std::string& path_prefix, const std::string& glob) {
    // First, find matching entries
    std::string sql = "SELECT path, filename FROM images WHERE 1=1";
    if (!path_prefix.empty()) {
        sql += " AND path LIKE ? || '%'";
    }
    if (!glob.empty()) {
        // Convert glob to SQL LIKE: * -> %, ? -> _
        std::string like_pattern;
        for (char c : glob) {
            if (c == '*') like_pattern += '%';
            else if (c == '?') like_pattern += '_';
            else if (c == '%' || c == '_') { like_pattern += '\\'; like_pattern += c; }
            else like_pattern += c;
        }
        sql += " AND filename LIKE ? ESCAPE '\\'";
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    int bind_idx = 1;
    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, bind_idx++, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!glob.empty()) {
        std::string like_pattern;
        for (char c : glob) {
            if (c == '*') like_pattern += '%';
            else if (c == '?') like_pattern += '_';
            else if (c == '%' || c == '_') { like_pattern += '\\'; like_pattern += c; }
            else like_pattern += c;
        }
        sqlite3_bind_text(stmt, bind_idx++, like_pattern.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<std::string> matched_paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) matched_paths.emplace_back(text);
    }
    sqlite3_finalize(stmt);

    return matched_paths;
}

int64_t Database::count() {
    const char* sql = "SELECT COUNT(*) FROM images";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare count: " + std::string(sqlite3_errmsg(db_)));
    }

    rc = sqlite3_step(stmt);
    int64_t result = 0;
    if (rc == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return result;
}

std::vector<ImageInfo> Database::get_all_images(const std::string& path_prefix) {
    std::string sql = "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif FROM images";
    if (!path_prefix.empty()) {
        sql += " WHERE path LIKE ? || '%'";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.exif_json = text(10);
        results.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<std::vector<ImageInfo>> Database::get_exact_duplicates(const std::string& path_prefix) {
    std::string sql;
    if (path_prefix.empty()) {
        sql = R"(
            SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif
            FROM images
            WHERE sha256 IN (SELECT sha256 FROM images GROUP BY sha256 HAVING COUNT(*) > 1)
            ORDER BY sha256, path
        )";
    } else {
        sql = R"(
            SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif
            FROM images
            WHERE path LIKE ? || '%'
              AND sha256 IN (SELECT sha256 FROM images WHERE path LIKE ? || '%' GROUP BY sha256 HAVING COUNT(*) > 1)
            ORDER BY sha256, path
        )";
    }
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (!path_prefix.empty()) {
        sqlite3_bind_text(stmt, 1, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, path_prefix.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> flat;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.exif_json = text(10);
        flat.push_back(std::move(info));
    }
    sqlite3_finalize(stmt);

    // Group by sha256
    std::vector<std::vector<ImageInfo>> groups;
    for (size_t i = 0; i < flat.size(); ) {
        size_t j = i;
        while (j < flat.size() && flat[j].sha256 == flat[i].sha256) j++;
        groups.emplace_back(flat.begin() + i, flat.begin() + j);
        i = j;
    }
    return groups;
}

int64_t Database::get_image_id(const std::string& path) {
    const char* sql = "SELECT id FROM images WHERE path = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, path.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int64_t id = -1;
    if (rc == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

bool Database::has_faces(const std::string& path) {
    int64_t image_id = get_image_id(path);
    if (image_id < 0) return false;

    const char* sql = "SELECT COUNT(*) FROM faces WHERE image_id = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, image_id);
    rc = sqlite3_step(stmt);
    int64_t count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count > 0;
}

void Database::delete_faces_for_image(int64_t image_id) {
    // Delete from vec_faces first (references face ids)
    const char* vec_sql = "DELETE FROM vec_faces WHERE face_id IN (SELECT id FROM faces WHERE image_id = ?)";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, vec_sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, image_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Delete from faces
    const char* sql = "DELETE FROM faces WHERE image_id = ?";
    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, image_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::insert_face(int64_t image_id, const FaceInfo& face) {
    // Insert into faces table
    const char* sql = R"(
        INSERT INTO faces (image_id, embedding, x, y, width, height)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face insert: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_int64(stmt, 1, image_id);
    sqlite3_bind_blob(stmt, 2, face.embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, face.x);
    sqlite3_bind_int(stmt, 4, face.y);
    sqlite3_bind_int(stmt, 5, face.width);
    sqlite3_bind_int(stmt, 6, face.height);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert face: " + std::string(sqlite3_errmsg(db_)));
    }

    // Get the face id
    int64_t face_id = sqlite3_last_insert_rowid(db_);

    // Insert into vec_faces for KNN search
    const char* vec_sql = "INSERT INTO vec_faces(face_id, embedding) VALUES (?, ?)";
    rc = sqlite3_prepare_v2(db_, vec_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare vec insert: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_int64(stmt, 1, face_id);
    sqlite3_bind_blob(stmt, 2, face.embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to insert vec face: " + std::string(sqlite3_errmsg(db_)));
    }
}

std::vector<Database::FaceSearchResult> Database::search_faces(
    const std::array<float, 128>& embedding, float threshold, int limit) {

    const char* sql = R"(
        SELECT f.id, i.path, v.distance, f.x, f.y, f.width, f.height
        FROM vec_faces v
        JOIN faces f ON f.id = v.face_id
        JOIN images i ON i.id = f.image_id
        WHERE v.embedding MATCH ?
          AND k = ?
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face search: " + std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_blob(stmt, 1, embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 100);

    std::vector<FaceSearchResult> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        float dist = static_cast<float>(sqlite3_column_double(stmt, 2));
        if (dist > threshold) continue;

        FaceSearchResult r;
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        r.path = path ? path : "";
        r.distance = dist;
        r.face_x = sqlite3_column_int(stmt, 3);
        r.face_y = sqlite3_column_int(stmt, 4);
        r.face_w = sqlite3_column_int(stmt, 5);
        r.face_h = sqlite3_column_int(stmt, 6);
        results.push_back(std::move(r));
    }

    sqlite3_finalize(stmt);
    return results;
}

int64_t Database::get_or_create_person(const std::string& name) {
    // Try to find existing
    const char* sql = "SELECT id FROM people WHERE name = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);

    // Create new
    const char* insert_sql = "INSERT INTO people (name) VALUES (?)";
    rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare insert: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Failed to create person: " + std::string(sqlite3_errmsg(db_)));
    }
    return sqlite3_last_insert_rowid(db_);
}

void Database::set_face_person(int64_t face_id, int64_t person_id) {
    const char* sql = "UPDATE faces SET person_id = ? WHERE id = ?";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare update: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_int64(stmt, 1, person_id);
    sqlite3_bind_int64(stmt, 2, face_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

int64_t Database::find_closest_face(const std::array<float, 128>& embedding, float threshold) {
    // Use vec_faces to find nearest face
    const char* sql = R"(
        SELECT face_id, distance FROM vec_faces
        WHERE embedding MATCH ? AND k = 1
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare face query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_blob(stmt, 1, embedding.data(), 128 * sizeof(float), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    int64_t face_id = -1;
    if (rc == SQLITE_ROW) {
        float dist = static_cast<float>(sqlite3_column_double(stmt, 1));
        if (dist <= threshold) {
            face_id = sqlite3_column_int64(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    return face_id;
}

std::vector<Database::PersonInfo> Database::list_people() {
    const char* sql = R"(
        SELECT p.id, p.name, COUNT(f.id) as face_count
        FROM people p
        LEFT JOIN faces f ON f.person_id = p.id
        GROUP BY p.id
        ORDER BY p.name
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }

    std::vector<PersonInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersonInfo p;
        p.id = sqlite3_column_int64(stmt, 0);
        const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        p.name = name ? name : "";
        p.face_count = sqlite3_column_int64(stmt, 2);
        results.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<std::string> Database::get_images_for_person(const std::string& name) {
    const char* sql = R"(
        SELECT DISTINCT i.path
        FROM images i
        JOIN faces f ON f.image_id = i.id
        JOIN people p ON p.id = f.person_id
        WHERE p.name = ?
        ORDER BY i.path
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare query: " + std::string(sqlite3_errmsg(db_)));
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<std::string> paths;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (path) paths.emplace_back(path);
    }
    sqlite3_finalize(stmt);
    return paths;
}

std::string Database::auto_label_face(int64_t face_id, const std::array<float, 128>& embedding,
                                       float threshold) {
    // Get all reference embeddings from named faces
    const char* sql = R"(
        SELECT f.embedding, p.id, p.name
        FROM faces f
        JOIN people p ON p.id = f.person_id
        WHERE f.person_id IS NOT NULL
    )";
    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return "";

    float best_dist = threshold;
    int64_t best_person_id = -1;
    std::string best_name;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const float* ref_data = static_cast<const float*>(sqlite3_column_blob(stmt, 0));
        if (!ref_data) continue;

        // Compute distance
        float dist = 0;
        for (int i = 0; i < 128; i++) {
            float diff = embedding[i] - ref_data[i];
            dist += diff * diff;
        }
        dist = std::sqrt(dist);

        if (dist < best_dist) {
            best_dist = dist;
            best_person_id = sqlite3_column_int64(stmt, 1);
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            best_name = name ? name : "";
        }
    }
    sqlite3_finalize(stmt);

    if (best_person_id >= 0) {
        set_face_person(face_id, best_person_id);
        return best_name;
    }
    return "";
}

std::vector<ImageInfo> Database::search(const SearchCriteria& c) {
    std::string sql = "SELECT path, filename, extension, file_size, created_at, modified_at, sha256, phash, width, height, exif FROM images WHERE 1=1";
    std::vector<std::pair<std::string, bool>> binds; // {value, is_int}

    if (!c.extension.empty()) {
        sql += " AND LOWER(extension) = LOWER(?)";
        binds.push_back({c.extension, false});
    }
    if (!c.after.empty()) {
        // Compare against best available date
        std::string exif_date = c.after;
        if (exif_date.size() >= 10) { exif_date[4] = ':'; exif_date[7] = ':'; }
        sql += " AND COALESCE(json_extract(exif, '$.DateTimeOriginal'), json_extract(exif, '$.DateTimeDigitized'), modified_at) >= ?";
        binds.push_back({exif_date, false});
    }
    if (!c.before.empty()) {
        std::string exif_date = c.before;
        if (exif_date.size() >= 10) { exif_date[4] = ':'; exif_date[7] = ':'; }
        sql += " AND COALESCE(json_extract(exif, '$.DateTimeOriginal'), json_extract(exif, '$.DateTimeDigitized'), modified_at) <= ?";
        binds.push_back({exif_date, false});
    }
    if (!c.camera.empty()) {
        sql += " AND json_extract(exif, '$.Model') LIKE '%' || ? || '%'";
        binds.push_back({c.camera, false});
    }
    if (!c.make.empty()) {
        sql += " AND json_extract(exif, '$.Make') LIKE '%' || ? || '%'";
        binds.push_back({c.make, false});
    }
    if (c.min_size >= 0) {
        sql += " AND file_size >= " + std::to_string(c.min_size);
    }
    if (c.max_size >= 0) {
        sql += " AND file_size <= " + std::to_string(c.max_size);
    }

    sql += " ORDER BY path";

    if (c.limit > 0) {
        sql += " LIMIT " + std::to_string(c.limit);
    }

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare search: " + std::string(sqlite3_errmsg(db_)));
    }

    for (size_t i = 0; i < binds.size(); i++) {
        sqlite3_bind_text(stmt, i + 1, binds[i].first.c_str(), -1, SQLITE_TRANSIENT);
    }

    std::vector<ImageInfo> results;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        ImageInfo info;
        auto text = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        info.path = text(0);
        info.filename = text(1);
        info.extension = text(2);
        info.file_size = sqlite3_column_int64(stmt, 3);
        info.created_at = text(4);
        info.modified_at = text(5);
        info.sha256 = text(6);
        info.phash = static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
        info.width = sqlite3_column_int(stmt, 8);
        info.height = sqlite3_column_int(stmt, 9);
        info.exif_json = text(10);
        results.push_back(std::move(info));
    }

    sqlite3_finalize(stmt);
    return results;
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}
