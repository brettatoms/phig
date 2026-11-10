#include <glib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A database of three rows dated 2021, 2023 and 2022 in insert order, so the
// gallery's newest-first order differs from insertion order. The 2023 row is a
// real JPEG, so a thumbnail can be generated from it. XDG_DATA_HOME needs no
// redirect: the catalog is the temp dir, so generated thumbnails land inside it.
struct Fixture {
    fs::path dir;
    std::string db_path;

    Fixture() {
        char* tmp = g_dir_make_tmp("phig-gtk-collection-XXXXXX", nullptr);
        g_assert_nonnull(tmp);
        dir = tmp;
        g_free(tmp);
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

    phig::gtk::CollectionPtr open() const {
        auto coll = phig::gtk::open(dir.string());
        if (!coll) g_error("open failed: %s", coll.error().c_str());
        return std::move(*coll);
    }
};

void test_open_reports_a_missing_catalog_without_creating_it() {
    Fixture f;
    fs::path absent = f.dir / "absent";
    auto coll = phig::gtk::open(absent.string());
    g_assert_false(coll.has_value());
    g_assert_false(coll.error().empty());
    g_assert_false(fs::exists(absent));
}

void test_pages_come_newest_first() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();

    auto n = phig::gtk::count(*coll);
    g_assert_true(n.has_value());
    g_assert_cmpuint(*n, ==, 3);

    const std::string sha_b(64, 'b');
    const std::string sha_c(64, 'c');
    auto page = phig::gtk::page(*coll, 0, 2);
    g_assert_true(page.has_value());
    g_assert_cmpuint(page->size(), ==, 2);
    g_assert_cmpstr((*page)[0].sha256.c_str(), ==, sha_b.c_str());
    g_assert_cmpstr((*page)[0].filename.c_str(), ==, "faces_small.jpg");
    g_assert_cmpstr((*page)[1].sha256.c_str(), ==, sha_c.c_str());

    page = phig::gtk::page(*coll, 2, 2);
    g_assert_true(page.has_value());
    g_assert_cmpuint(page->size(), ==, 1);
    g_assert_cmpstr((*page)[0].path.c_str(), ==, "/nowhere/a.jpg");

    page = phig::gtk::page(*coll, 3, 2);
    g_assert_true(page.has_value());
    g_assert_true(page->empty());
}

void test_a_thumbnail_is_generated_on_demand() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::string sha(64, 'b');

    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Absent);
    g_assert_true(g_str_has_prefix(found.path.c_str(), f.dir.c_str()));

    const int64_t id = phig::gtk::page(*coll, 0, 1)->front().id;
    g_assert_true(phig::gtk::thumb_generate(*coll, id, 256) == phig::gtk::ThumbGenerate::Ok);

    found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Present);
    g_assert_true(g_file_test(found.path.c_str(), G_FILE_TEST_EXISTS));
    g_assert_true(found.path.rfind((f.dir / "thumbs").string(), 0) == 0);
}

void test_generation_names_a_missing_source_and_an_unknown_id() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const int64_t missing = phig::gtk::page(*coll, 2, 1)->front().id;  // /nowhere/a.jpg
    g_assert_true(phig::gtk::thumb_generate(*coll, missing, 256) ==
                  phig::gtk::ThumbGenerate::SourceMissing);
    g_assert_true(phig::gtk::thumb_generate(*coll, 999999, 256) ==
                  phig::gtk::ThumbGenerate::NoSuchRow);
}

void test_lookup_rejects_a_malformed_sha() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*coll, "abc", 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Invalid);
    g_assert_true(found.path.empty());
}

void test_default_catalog_follows_the_environment() {
    // A config directory with no config file, so a developer's real
    // config.ini cannot decide the answer.
    const std::string config_home =
        "/tmp/phig-gtk-xdg-config-" + std::to_string(static_cast<long>(getpid()));
    g_setenv("XDG_CONFIG_HOME", config_home.c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    g_unsetenv("PHIG_CATALOG");

    auto dir = phig::gtk::default_catalog();
    g_assert_true(dir.has_value());
    g_assert_cmpstr(dir->c_str(), ==, "/tmp/phig-gtk-xdg-data/phig");

    g_setenv("PHIG_CATALOG", "/tmp/phig-gtk-elsewhere", TRUE);
    dir = phig::gtk::default_catalog();
    g_assert_true(dir.has_value());
    g_assert_cmpstr(dir->c_str(), ==, "/tmp/phig-gtk-elsewhere");

    g_unsetenv("PHIG_CATALOG");
    g_unsetenv("XDG_DATA_HOME");
    g_unsetenv("XDG_CONFIG_HOME");
}

void test_default_catalog_reports_why_it_cannot_be_resolved() {
    const fs::path config_home =
        "/tmp/phig-gtk-xdg-config-empty-" + std::to_string(static_cast<long>(getpid()));
    fs::create_directories(config_home / "phig");
    std::ofstream(config_home / "phig" / "config.ini") << "[paths]\ncatalog =\n";
    g_setenv("XDG_CONFIG_HOME", config_home.c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", "/tmp/phig-gtk-xdg-data", TRUE);
    g_unsetenv("PHIG_CATALOG");

    auto dir = phig::gtk::default_catalog();
    g_assert_false(dir.has_value());
    g_assert_false(dir.error().empty());

    g_unsetenv("XDG_DATA_HOME");
    g_unsetenv("XDG_CONFIG_HOME");
    std::error_code ec;
    fs::remove_all(config_home, ec);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/collection/open-reports-a-missing-catalog-without-creating-it",
                    test_open_reports_a_missing_catalog_without_creating_it);
    g_test_add_func("/collection/pages-come-newest-first", test_pages_come_newest_first);
    g_test_add_func("/collection/a-thumbnail-is-generated-on-demand",
                    test_a_thumbnail_is_generated_on_demand);
    g_test_add_func("/collection/generation-names-a-missing-source-and-an-unknown-id",
                    test_generation_names_a_missing_source_and_an_unknown_id);
    g_test_add_func("/collection/lookup-rejects-a-malformed-sha",
                    test_lookup_rejects_a_malformed_sha);
    g_test_add_func("/collection/default-catalog-follows-the-environment",
                    test_default_catalog_follows_the_environment);
    g_test_add_func("/collection/default-catalog-reports-why-it-cannot-be-resolved",
                    test_default_catalog_reports_why_it_cannot_be_resolved);
    return g_test_run();
}
