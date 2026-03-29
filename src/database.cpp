#include "database.h"
#include <filesystem>
#include <stdexcept>
#include <iostream>

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

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL error: " + msg);
    }
}
