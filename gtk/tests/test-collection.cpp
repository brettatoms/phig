#include <glib.h>

#include <filesystem>
#include <optional>
#include <string>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A database of three rows dated 2021, 2023 and 2022 in insert order, so the
// gallery's newest-first order differs from insertion order. The 2023 row is a
// real JPEG, so a thumbnail can be generated from it. XDG_CACHE_HOME points
// into the temp dir, so generated thumbnails never reach the real cache.
struct Fixture {
    fs::path dir;
    std::string db_path;
    std::optional<std::string> saved_cache_home;  // XDG_CACHE_HOME before the fixture

    Fixture() {
        char* tmp = g_dir_make_tmp("phig-gtk-collection-XXXXXX", nullptr);
        g_assert_nonnull(tmp);
        dir = tmp;
        g_free(tmp);
        if (const char* cache_home = g_getenv("XDG_CACHE_HOME")) saved_cache_home = cache_home;
        g_setenv("XDG_CACHE_HOME", (dir / "cache").c_str(), TRUE);
        db_path = (dir / "phig.db").string();

        Database db(db_path);
        db.init_schema();
        insert(db, "/nowhere/a.jpg", "2021-01-01T00:00:00Z", std::string(64, 'a'));
        insert(db, PHIG_SOURCE_DIR "/tests/data/faces_small.jpg", "2023-01-01T00:00:00Z",
               std::string(64, 'b'));
        insert(db, "/nowhere/c.jpg", "2022-01-01T00:00:00Z", std::string(64, 'c'));
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
        if (saved_cache_home)
            g_setenv("XDG_CACHE_HOME", saved_cache_home->c_str(), TRUE);
        else
            g_unsetenv("XDG_CACHE_HOME");
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    static void insert(Database& db, const std::string& path, const char* modified_at,
                       const std::string& sha) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = "jpg";
        info.modified_at = modified_at;
        info.sha256 = sha;
        db.insert(info);
    }

    PhigCollection* open() const {
        char* error = nullptr;
        PhigCollection* coll = phig_collection_open(db_path.c_str(), &error);
        if (coll == nullptr) g_error("open failed: %s", error);
        return coll;
    }
};

void test_open_reports_a_missing_file_without_creating_it() {
    Fixture f;
    fs::path absent = f.dir / "absent.db";
    char* error = nullptr;
    PhigCollection* coll = phig_collection_open(absent.c_str(), &error);
    g_assert_null(coll);
    g_assert_nonnull(error);
    g_assert_cmpstr(error, !=, "");
    g_free(error);
    g_assert_false(fs::exists(absent));
}

void test_pages_come_newest_first() {
    Fixture f;
    PhigCollection* coll = f.open();

    guint n = 0;
    g_assert_true(phig_collection_count(coll, &n));
    g_assert_cmpuint(n, ==, 3);

    const std::string sha_b(64, 'b');
    const std::string sha_c(64, 'c');
    PhigRowPage* page = phig_collection_page(coll, 0, 2);
    g_assert_nonnull(page);
    g_assert_cmpuint(page->n_rows, ==, 2);
    g_assert_cmpstr(page->rows[0].sha256, ==, sha_b.c_str());
    g_assert_cmpstr(page->rows[0].filename, ==, "faces_small.jpg");
    g_assert_cmpstr(page->rows[1].sha256, ==, sha_c.c_str());
    phig_row_page_free(page);

    page = phig_collection_page(coll, 2, 2);
    g_assert_cmpuint(page->n_rows, ==, 1);
    g_assert_cmpstr(page->rows[0].path, ==, "/nowhere/a.jpg");
    phig_row_page_free(page);

    page = phig_collection_page(coll, 3, 2);
    g_assert_nonnull(page);
    g_assert_cmpuint(page->n_rows, ==, 0);
    phig_row_page_free(page);

    phig_collection_close(coll);
}

void test_a_thumbnail_is_generated_on_demand() {
    Fixture f;
    PhigCollection* coll = f.open();
    std::string sha(64, 'b');

    char* path = nullptr;
    g_assert_cmpint(phig_collection_thumb_lookup(coll, sha.c_str(), 256, &path), ==,
                    PHIG_THUMB_ABSENT);
    g_assert_nonnull(path);
    g_assert_true(g_str_has_prefix(path, f.dir.c_str()));
    g_free(path);

    PhigRowPage* page = phig_collection_page(coll, 0, 1);
    int64_t id = page->rows[0].id;
    phig_row_page_free(page);

    g_assert_true(phig_collection_thumb_generate(coll, id, 256));
    g_assert_cmpint(phig_collection_thumb_lookup(coll, sha.c_str(), 256, &path), ==,
                    PHIG_THUMB_PRESENT);
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    g_free(path);

    phig_collection_close(coll);
}

void test_generation_fails_for_a_missing_source_or_an_unknown_id() {
    Fixture f;
    PhigCollection* coll = f.open();
    PhigRowPage* page = phig_collection_page(coll, 2, 1);  // the /nowhere/a.jpg row
    g_assert_false(phig_collection_thumb_generate(coll, page->rows[0].id, 256));
    phig_row_page_free(page);
    g_assert_false(phig_collection_thumb_generate(coll, 999999, 256));
    phig_collection_close(coll);
}

void test_lookup_rejects_a_malformed_sha() {
    Fixture f;
    PhigCollection* coll = f.open();
    char* path = nullptr;
    g_assert_cmpint(phig_collection_thumb_lookup(coll, "abc", 256, &path), ==, PHIG_THUMB_ERROR);
    g_assert_null(path);
    phig_collection_close(coll);
}

void test_default_path_follows_xdg_data_home() {
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    char* path = phig_collection_default_path();
    g_assert_cmpstr(path, ==, "/tmp/phig-gtk-xdg-data/phig/phig.db");
    g_free(path);
    g_unsetenv("XDG_DATA_HOME");
    g_assert_false(g_file_test("/tmp/phig-gtk-xdg-data", G_FILE_TEST_EXISTS));
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/collection/open-reports-a-missing-file-without-creating-it",
                    test_open_reports_a_missing_file_without_creating_it);
    g_test_add_func("/collection/pages-come-newest-first", test_pages_come_newest_first);
    g_test_add_func("/collection/a-thumbnail-is-generated-on-demand",
                    test_a_thumbnail_is_generated_on_demand);
    g_test_add_func("/collection/generation-fails-for-a-missing-source-or-an-unknown-id",
                    test_generation_fails_for_a_missing_source_or_an_unknown_id);
    g_test_add_func("/collection/lookup-rejects-a-malformed-sha",
                    test_lookup_rejects_a_malformed_sha);
    g_test_add_func("/collection/default-path-follows-xdg-data-home",
                    test_default_path_follows_xdg_data_home);
    return g_test_run();
}
