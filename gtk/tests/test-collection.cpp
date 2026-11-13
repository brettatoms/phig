#include <glib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// A database of three rows dated 2021, 2023 and 2022 in insert order, so the
// gallery's newest-first order differs from insertion order:
//
//   a  /nowhere/a.jpg                     2021  300 bytes  Canon EOS R5  image
//   b  tests/data/faces_small.jpg         2023  100 bytes                image
//   c  /nowhere/c.mov                     2022  200 bytes  Apple iPhone  video
//
// b is a real JPEG, so a thumbnail can be generated from it. XDG_DATA_HOME
// needs no redirect: the catalog is the temp dir, so generated thumbnails land
// inside it.
struct Fixture {
    fs::path dir;
    std::string db_path;
    int64_t a = 0, b = 0, c = 0;

    Fixture() {
        char* tmp = g_dir_make_tmp("phig-gtk-collection-XXXXXX", nullptr);
        g_assert_nonnull(tmp);
        dir = tmp;
        g_free(tmp);
        db_path = (dir / "phig.db").string();

        Database db(db_path);
        db.init_schema();
        a = insert(db, "/nowhere/a.jpg", "2021-01-01T00:00:00Z", std::string(64, 'a'), 300,
                   R"({"Make":"Canon","Model":"Canon EOS R5"})", "image");
        b = insert(db, PHIG_SOURCE_DIR "/tests/data/faces_small.jpg", "2023-01-01T00:00:00Z",
                   std::string(64, 'b'), 100, "{}", "image");
        c = insert(db, "/nowhere/c.mov", "2022-01-01T00:00:00Z", std::string(64, 'c'), 200,
                   R"({"Make":"Apple","Model":"iPhone 15"})", "video");
    }

    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    static int64_t insert(Database& db, const std::string& path, const char* modified_at,
                          const std::string& sha, int64_t size, const std::string& metadata,
                          const char* media) {
        ImageInfo info;
        info.path = path;
        info.filename = fs::path(path).filename().string();
        info.extension = fs::path(path).extension().string().substr(1);
        info.modified_at = modified_at;
        info.sha256 = sha;
        info.file_size = size;
        info.metadata_json = metadata;
        info.media_type = media;
        db.insert(info);
        return db.get_image_id(path);
    }

    phig::gtk::CollectionPtr open() const {
        auto coll = phig::gtk::open(dir.string());
        if (!coll) g_error("open failed: %s", coll.error().message.c_str());
        return std::move(*coll);
    }
};

std::vector<int64_t> ids_for(phig::gtk::Collection& coll, const phig::gtk::Query& query) {
    auto found = phig::gtk::ids(coll, query);
    if (!found) g_error("ids failed: %s", found.error().c_str());
    return *found;
}

// The ids of a default query that set adjusts.
template <typename Set>
std::vector<int64_t> ids_where(phig::gtk::Collection& coll, const Set& set) {
    phig::gtk::Query query;
    set(query);
    return ids_for(coll, query);
}

void assert_ids(const std::vector<int64_t>& got, const std::vector<int64_t>& want) {
    g_assert_cmpuint(got.size(), ==, want.size());
    for (size_t i = 0; i < want.size(); i++) g_assert_cmpint(got[i], ==, want[i]);
}

void test_open_reports_a_missing_catalog_without_creating_it() {
    Fixture f;
    fs::path absent = f.dir / "absent";
    auto coll = phig::gtk::open(absent.string());
    g_assert_false(coll.has_value());
    g_assert_true(coll.error().missing);
    g_assert_false(coll.error().message.empty());
    g_assert_false(fs::exists(absent));
}

void test_ids_come_newest_first_by_default() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    assert_ids(ids_for(*coll, {}), {f.b, f.c, f.a});
}

void test_each_sort_orders_the_rows() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    using phig::gtk::Sort;
    auto sorted = [&](Sort sort) {
        return ids_where(*coll, [sort](phig::gtk::Query& q) { q.sort = sort; });
    };
    assert_ids(sorted(Sort::DateOldest), {f.a, f.c, f.b});
    assert_ids(sorted(Sort::Filename), {f.a, f.c, f.b});  // a.jpg, c.mov, faces_small.jpg
    assert_ids(sorted(Sort::Size), {f.a, f.c, f.b});      // largest first
}

void test_rows_come_in_the_order_asked_and_skip_unknown_ids() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::vector<int64_t> asked = {f.c, 999999, f.a};
    auto rows = phig::gtk::rows(*coll, asked);
    g_assert_true(rows.has_value());
    g_assert_cmpuint(rows->size(), ==, 2);
    g_assert_cmpint((*rows)[0].id, ==, f.c);
    g_assert_cmpstr((*rows)[0].path.c_str(), ==, "/nowhere/c.mov");
    g_assert_cmpstr((*rows)[0].filename.c_str(), ==, "c.mov");
    g_assert_cmpint((*rows)[1].id, ==, f.a);
    const std::string sha_a(64, 'a');
    g_assert_cmpstr((*rows)[1].sha256.c_str(), ==, sha_a.c_str());
}

void test_each_filter_narrows_and_they_combine() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    {
        Database db(f.db_path);
        db.set_tag_by_ids({f.a, f.b}, "trip", true);
        db.set_tag_by_ids({f.a}, "beach", true);
        db.set_tag_by_ids({f.b}, "favorite", true);
        db.set_gallery_by_ids({f.c}, "Clips");
    }
    using phig::gtk::Query;
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.tags = {"trip"}; }), {f.b, f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.tags = {"trip", "beach"}; }), {f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.favorite = true; }), {f.b});
    // A favorite filter and a tag filter naming the favorite tag ask for it once.
    assert_ids(ids_where(*coll,
                         [](Query& q) {
                             q.filter.favorite = true;
                             q.filter.tags = {"favorite", "trip"};
                         }),
               {f.b});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.gallery = "Clips"; }), {f.c});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.after = "2022-01-01"; }), {f.b, f.c});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.before = "2022-01-01"; }), {f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.camera = "EOS"; }), {f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.make = "Apple"; }), {f.c});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.media = "video"; }), {f.c});
    assert_ids(ids_where(*coll, [](Query& q) { q.filter.extension = "JPG"; }), {f.b, f.a});
    assert_ids(ids_where(*coll,
                         [](Query& q) {
                             q.filter.tags = {"trip"};
                             q.filter.after = "2022-01-01";
                         }),
               {f.b});
}

void test_search_matches_text_anywhere_in_the_path_or_a_glob() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    using phig::gtk::Query;
    assert_ids(ids_where(*coll, [](Query& q) { q.search = "faces"; }), {f.b});
    assert_ids(ids_where(*coll, [](Query& q) { q.search = "nowhere"; }), {f.c, f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.search = "*.mov"; }), {f.c});
    assert_ids(ids_where(*coll, [](Query& q) { q.search = "a.jpg"; }), {f.a});
    assert_ids(ids_where(*coll, [](Query& q) { q.search = "A.JPG"; }), {});
}

void test_counts_list_galleries_and_tags_but_not_favorite() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    {
        Database db(f.db_path);
        db.set_tag_by_ids({f.a, f.b}, "trip", true);
        db.set_tag_by_ids({f.a}, "gone", true);
        db.set_tag_by_ids({f.a}, "gone", false);  // a tag row no image carries
        db.set_tag_by_ids({f.b, f.c}, "favorite", true);
        db.set_gallery_by_ids({f.a, f.c}, "Travel");
        db.set_gallery_by_ids({f.b}, "Home");
    }
    auto counts = phig::gtk::counts(*coll);
    g_assert_true(counts.has_value());
    g_assert_cmpuint(counts->all, ==, 3);
    g_assert_cmpuint(counts->favorites, ==, 2);
    g_assert_cmpuint(counts->galleries.size(), ==, 2);
    g_assert_cmpstr(counts->galleries[0].name.c_str(), ==, "Home");
    g_assert_cmpuint(counts->galleries[0].count, ==, 1);
    g_assert_cmpstr(counts->galleries[1].name.c_str(), ==, "Travel");
    g_assert_cmpuint(counts->galleries[1].count, ==, 2);
    g_assert_cmpuint(counts->tags.size(), ==, 1);
    g_assert_cmpstr(counts->tags[0].name.c_str(), ==, "trip");
    g_assert_cmpuint(counts->tags[0].count, ==, 2);
}

void test_details_carry_what_the_viewer_shows() {
    Fixture f;
    int64_t id = 0;
    {
        Database db(f.db_path);
        ImageInfo info;
        info.path = "/nowhere/exif.jpg";
        info.filename = "exif.jpg";
        info.extension = "jpg";
        info.modified_at = "2026-11-13T20:25:35Z";
        info.sha256 = std::string(64, 'e');
        info.file_size = 2048;
        info.width = 200;
        info.height = 100;
        info.blur_score = 6.5;
        info.metadata_json =
            R"({"Make":"Canon","Model":"Canon EOS R5","ExposureTime":"1/500 sec.",)"
            R"("FNumber":"f/4.0","ISOSpeedRatings":"400","FocalLength":"24.0 mm",)"
            R"("DateTimeOriginal":"2026:08:08 14:32:11"})";
        db.insert(info);
        id = db.get_image_id(info.path);
        db.set_tag_by_ids({id}, "trip", true);
        db.set_tag_by_ids({id}, "favorite", true);
        db.set_gallery_by_ids({id}, "Travel");
    }
    phig::gtk::CollectionPtr coll = f.open();
    auto d = phig::gtk::details(*coll, id);
    g_assert_true(d.has_value());
    g_assert_cmpint(d->id, ==, id);
    g_assert_cmpstr(d->path.c_str(), ==, "/nowhere/exif.jpg");
    g_assert_cmpstr(d->filename.c_str(), ==, "exif.jpg");
    g_assert_cmpstr(d->media_type.c_str(), ==, "image");
    g_assert_cmpstr(d->date.c_str(), ==, "2026-08-08 14:32:11");
    g_assert_cmpstr(d->make.c_str(), ==, "Canon");
    g_assert_cmpstr(d->model.c_str(), ==, "Canon EOS R5");
    g_assert_cmpstr(d->exposure_time.c_str(), ==, "1/500 sec.");
    g_assert_cmpstr(d->f_number.c_str(), ==, "f/4.0");
    g_assert_cmpstr(d->iso.c_str(), ==, "400");
    g_assert_cmpstr(d->focal_length.c_str(), ==, "24.0 mm");
    g_assert_cmpint(d->width, ==, 200);
    g_assert_cmpint(d->height, ==, 100);
    g_assert_cmpint(d->file_size, ==, 2048);
    g_assert_cmpfloat(d->blur_score, ==, 6.5);
    g_assert_cmpuint(d->tags.size(), ==, 1);
    g_assert_cmpstr(d->tags[0].c_str(), ==, "trip");
    g_assert_cmpstr(d->gallery.c_str(), ==, "Travel");
    g_assert_true(d->favorite);

    g_assert_false(phig::gtk::details(*coll, 999999).has_value());
}

void test_a_thumbnail_is_generated_on_demand() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::string sha(64, 'b');

    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Absent);
    g_assert_true(g_str_has_prefix(found.path.c_str(), f.dir.c_str()));

    g_assert_true(phig::gtk::thumb_generate(*coll, f.b, 256) == phig::gtk::ThumbGenerate::Ok);

    found = phig::gtk::thumb_lookup(*coll, sha, 256);
    g_assert_true(found.state == phig::gtk::ThumbLookup::Present);
    g_assert_true(g_file_test(found.path.c_str(), G_FILE_TEST_EXISTS));
    g_assert_true(found.path.rfind((f.dir / "thumbs").string(), 0) == 0);
}

void test_generation_names_a_missing_source_and_an_unknown_id() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    g_assert_true(phig::gtk::thumb_generate(*coll, f.a, 256) ==
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
    g_test_add_func("/collection/ids-come-newest-first-by-default",
                    test_ids_come_newest_first_by_default);
    g_test_add_func("/collection/each-sort-orders-the-rows", test_each_sort_orders_the_rows);
    g_test_add_func("/collection/rows-come-in-the-order-asked-and-skip-unknown-ids",
                    test_rows_come_in_the_order_asked_and_skip_unknown_ids);
    g_test_add_func("/collection/each-filter-narrows-and-they-combine",
                    test_each_filter_narrows_and_they_combine);
    g_test_add_func("/collection/search-matches-text-anywhere-in-the-path-or-a-glob",
                    test_search_matches_text_anywhere_in_the_path_or_a_glob);
    g_test_add_func("/collection/counts-list-galleries-and-tags-but-not-favorite",
                    test_counts_list_galleries_and_tags_but_not_favorite);
    g_test_add_func("/collection/details-carry-what-the-viewer-shows",
                    test_details_carry_what_the_viewer_shows);
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
