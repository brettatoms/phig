#include <fcntl.h>
#include <glib.h>
#include <sqlite3.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <latch>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "collection.h"
#include "database.h"
#include "types.h"

namespace fs = std::filesystem;

namespace {

// g_assert_cmpstr keeps only the pointer it is given, so a temporary string
// must outlive the macro: here it lives until this call returns.
void assert_text(const std::string& got, const char* want) {
    g_assert_cmpstr(got.c_str(), ==, want);
}

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

    std::vector<std::string> tags_of(const std::string& path) const {
        Database db(db_path);
        return db.get_tags_for_image(path);
    }

    std::string gallery_of(int64_t id) const {
        Database db(db_path);
        return db.get_images_by_ids({id}).front().gallery;
    }

    bool favorite_of(int64_t id) const {
        Database db(db_path);
        return db.get_images_by_ids({id}).front().favorite;
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

// Turns a current database into a genuine v7 one: images and faces back to a
// plain INTEGER PRIMARY KEY, faces without the v10 provenance columns, version
// stamped 7. Mirrors rewind_to_v7 in tests/test_schema_upgrade.cpp.
void rewind_to_v7(sqlite3* db) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, R"(
        PRAGMA foreign_keys=OFF;
        BEGIN;
        CREATE TABLE images_old (
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
            metadata    TEXT,
            media_type  TEXT NOT NULL DEFAULT 'image',
            duration_ms INTEGER,
            gallery     TEXT,
            blur_score  REAL,
            effective_date TEXT
        );
        INSERT INTO images_old SELECT id, path, filename, extension, file_size, created_at,
            modified_at, sha256, phash, width, height, metadata, media_type, duration_ms,
            gallery, blur_score, effective_date FROM images;
        DROP TABLE images;
        ALTER TABLE images_old RENAME TO images;
        CREATE INDEX idx_phash ON images(phash);
        CREATE INDEX idx_sha256 ON images(sha256);
        CREATE INDEX idx_path ON images(path);
        CREATE INDEX idx_gallery ON images(gallery);
        CREATE INDEX idx_effective_date ON images(effective_date DESC, id DESC);
        CREATE TABLE faces_old (
            id          INTEGER PRIMARY KEY,
            image_id    INTEGER NOT NULL REFERENCES images(id) ON DELETE CASCADE,
            embedding   BLOB NOT NULL,
            x INTEGER, y INTEGER, width INTEGER, height INTEGER,
            person_id   INTEGER REFERENCES people(id)
        );
        INSERT INTO faces_old SELECT id, image_id, embedding, x, y, width, height, person_id
            FROM faces;
        DROP TABLE faces;
        ALTER TABLE faces_old RENAME TO faces;
        CREATE INDEX idx_faces_image ON faces(image_id);
        UPDATE meta SET value = '7' WHERE key = 'schema_version';
        COMMIT;
        PRAGMA foreign_keys=ON;
    )",
                          nullptr, nullptr, &err);
    if (rc != SQLITE_OK) g_error("rewind to v7 failed: %s", err ? err : "?");
    sqlite3_free(err);
}

void test_upgrade_pending_is_false_for_a_current_catalog() {
    Fixture f;
    auto pending = phig::gtk::upgrade_pending(f.dir.string());
    g_assert_true(pending.has_value());
    g_assert_false(*pending);
}

void test_upgrade_pending_is_false_for_a_missing_catalog_and_creates_nothing() {
    Fixture f;
    fs::path absent = f.dir / "absent";
    auto pending = phig::gtk::upgrade_pending(absent.string());
    g_assert_true(pending.has_value());
    g_assert_false(*pending);
    g_assert_false(fs::exists(absent));
}

void test_an_outdated_catalog_is_pending_until_open_upgrades_it() {
    Fixture f;
    {
        Database db(f.db_path);
        rewind_to_v7(db.db_ptr());
    }
    auto pending = phig::gtk::upgrade_pending(f.dir.string());
    g_assert_true(pending.has_value());
    g_assert_true(*pending);
    {
        Database v7(f.db_path, /*read_only=*/true);
        g_assert_cmpint(v7.schema_version(), ==, 7);  // the check changed nothing
    }

    phig::gtk::CollectionPtr coll = f.open();
    std::string backup = phig::gtk::upgrade_backup(*coll);
    g_assert_false(backup.empty());
    g_assert_true(fs::exists(backup));

    auto after = phig::gtk::upgrade_pending(f.dir.string());
    g_assert_true(after.has_value());
    g_assert_false(*after);
}

void test_open_of_a_current_catalog_has_no_backup() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    g_assert_true(phig::gtk::upgrade_backup(*coll).empty());
}

void test_ids_come_newest_first_by_default() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    assert_ids(ids_for(*coll, {}), {f.b, f.c, f.a});
}

void test_each_sort_orders_the_rows() {
    Fixture f;
    // A fourth row, so each sort gives an order the others do not:
    //   date oldest a c b d, filename a d c b, size largest first a c d b.
    int64_t d = 0;
    {
        Database db(f.db_path);
        d = Fixture::insert(db, "/nowhere/b.jpg", "2024-01-01T00:00:00Z", std::string(64, 'd'), 150,
                            "{}", "image");
    }
    phig::gtk::CollectionPtr coll = f.open();
    using phig::gtk::Sort;
    auto sorted = [&](Sort sort) {
        return ids_where(*coll, [sort](phig::gtk::Query& q) { q.sort = sort; });
    };
    assert_ids(sorted(Sort::DateNewest), {d, f.b, f.c, f.a});
    assert_ids(sorted(Sort::DateOldest), {f.a, f.c, f.b, d});
    assert_ids(sorted(Sort::Filename), {f.a, d, f.c, f.b});  // a, b, c.mov, faces_small
    assert_ids(sorted(Sort::Size), {f.a, f.c, d, f.b});      // 300, 200, 150, 100
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
    const std::string sha_e(64, 'e');
    g_assert_cmpstr(d->sha256.c_str(), ==, sha_e.c_str());
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

void test_writes_change_exactly_the_named_images() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::string path_a = "/nowhere/a.jpg", path_c = "/nowhere/c.mov";
    const std::string path_b = PHIG_SOURCE_DIR "/tests/data/faces_small.jpg";

    g_assert_true(phig::gtk::tag_add(*coll, {f.a, f.c}, "trip").has_value());
    g_assert_cmpuint(f.tags_of(path_a).size(), ==, 1);
    assert_text(f.tags_of(path_a).front(), "trip");
    g_assert_true(f.tags_of(path_b).empty());
    g_assert_cmpuint(f.tags_of(path_c).size(), ==, 1);

    g_assert_true(phig::gtk::tag_remove(*coll, {f.c}, "trip").has_value());
    g_assert_cmpuint(f.tags_of(path_a).size(), ==, 1);
    assert_text(f.tags_of(path_a).front(), "trip");
    g_assert_true(f.tags_of(path_c).empty());

    g_assert_true(phig::gtk::gallery_set(*coll, {f.b}, "Home").has_value());
    assert_text(f.gallery_of(f.b), "Home");
    g_assert_true(f.gallery_of(f.a).empty());
    g_assert_true(f.gallery_of(f.c).empty());

    g_assert_true(phig::gtk::favorite_set(*coll, {f.a}, true).has_value());
    g_assert_true(f.favorite_of(f.a));
    g_assert_false(f.favorite_of(f.b));
    g_assert_true(phig::gtk::favorite_set(*coll, {f.a}, false).has_value());
    g_assert_false(f.favorite_of(f.a));

    g_assert_true(phig::gtk::gallery_set(*coll, {f.a}, "Away").has_value());
    g_assert_true(phig::gtk::gallery_clear(*coll, {f.b}).has_value());
    g_assert_true(f.gallery_of(f.b).empty());
    assert_text(f.gallery_of(f.a), "Away");

    // An empty selection names nothing and writes nothing.
    g_assert_true(phig::gtk::tag_add(*coll, {}, "everything").has_value());
    g_assert_cmpuint(f.tags_of(path_b).size(), ==, 0);
}

void test_gallery_set_replaces_the_gallery_an_image_had() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    g_assert_true(phig::gtk::gallery_set(*coll, {f.a}, "First").has_value());
    g_assert_true(phig::gtk::gallery_set(*coll, {f.a}, "Second").has_value());
    assert_text(f.gallery_of(f.a), "Second");
}

void test_a_write_refuses_an_empty_name() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    auto tagged = phig::gtk::tag_add(*coll, {f.a}, "");
    g_assert_false(tagged.has_value());
    g_assert_false(tagged.error().busy);
    auto filed = phig::gtk::gallery_set(*coll, {f.a}, "");
    g_assert_false(filed.has_value());
    g_assert_true(f.tags_of("/nowhere/a.jpg").empty());
    g_assert_true(f.gallery_of(f.a).empty());
}

// The writer lock is an flock, which contends between open file descriptions,
// so a descriptor of the test's own stands in for another process, as
// LockfileTest does.
void test_a_write_is_busy_while_another_process_holds_the_writer_lock() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    const std::string lock_path = (f.dir / "phig.db.lock").string();
    int other = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    g_assert_cmpint(other, >=, 0);
    g_assert_cmpint(::flock(other, LOCK_EX | LOCK_NB), ==, 0);

    auto tagged = phig::gtk::tag_add(*coll, {f.a}, "blocked");
    g_assert_false(tagged.has_value());
    g_assert_true(tagged.error().busy);
    g_assert_false(tagged.error().message.empty());
    auto filed = phig::gtk::gallery_set(*coll, {f.a}, "Blocked");
    g_assert_false(filed.has_value());
    g_assert_true(filed.error().busy);
    auto faved = phig::gtk::favorite_set(*coll, {f.a}, true);
    g_assert_false(faved.has_value());
    g_assert_true(faved.error().busy);
    g_assert_false(f.favorite_of(f.a));
    g_assert_true(f.tags_of("/nowhere/a.jpg").empty());
    g_assert_true(f.gallery_of(f.a).empty());

    ::flock(other, LOCK_UN);
    ::close(other);
    g_assert_true(phig::gtk::tag_add(*coll, {f.a}, "allowed").has_value());
    g_assert_cmpuint(f.tags_of("/nowhere/a.jpg").size(), ==, 1);
}

// The writer lock contends between this process's own descriptors too, so two
// writes from the window's workers would see each other as another process
// without write_mutex. Each round releases two writes on different ids together.
void test_writes_started_together_in_this_process_both_land() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    constexpr int kRounds = 50;
    for (int round = 0; round < kRounds; round++) {
        const std::string tag = "round-" + std::to_string(round);
        std::latch start(2);
        phig::gtk::WriteResult first, second;
        std::thread one([&] {
            start.arrive_and_wait();
            first = phig::gtk::tag_add(*coll, {f.a}, tag);
        });
        std::thread two([&] {
            start.arrive_and_wait();
            second = phig::gtk::tag_add(*coll, {f.c}, tag);
        });
        one.join();
        two.join();
        for (const phig::gtk::WriteResult* r : {&first, &second})
            if (!r->has_value())
                g_error("round %d: busy=%d %s", round, r->error().busy, r->error().message.c_str());
    }
    g_assert_cmpuint(f.tags_of("/nowhere/a.jpg").size(), ==, kRounds);
    g_assert_cmpuint(f.tags_of("/nowhere/c.mov").size(), ==, kRounds);
}

// A CLI rm can delete a row between the click that captured its id and the
// write. The write skips that id and changes the others.
void test_a_write_skips_an_id_whose_row_was_deleted() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    {
        Database db(f.db_path);
        db.delete_paths({"/nowhere/c.mov"});
    }

    g_assert_true(phig::gtk::tag_add(*coll, {f.a, f.c}, "kept").has_value());
    g_assert_cmpuint(f.tags_of("/nowhere/a.jpg").size(), ==, 1);
    assert_text(f.tags_of("/nowhere/a.jpg").front(), "kept");
    g_assert_true(phig::gtk::gallery_set(*coll, {f.c, f.a}, "Still").has_value());
    assert_text(f.gallery_of(f.a), "Still");
    g_assert_true(phig::gtk::favorite_set(*coll, {f.a, f.c}, true).has_value());
    g_assert_true(f.favorite_of(f.a));
    g_assert_true(phig::gtk::tag_remove(*coll, {f.c, f.a}, "kept").has_value());
    g_assert_cmpuint(f.tags_of("/nowhere/a.jpg").size(), ==, 1);
    assert_text(f.tags_of("/nowhere/a.jpg").front(), "favorite");
    g_assert_true(phig::gtk::gallery_clear(*coll, {f.a, f.c}).has_value());
    g_assert_true(f.gallery_of(f.a).empty());

    // No write brought the deleted row back.
    assert_ids(ids_for(*coll, {}), {f.b, f.a});
}

void test_an_image_decodes_upright_within_its_max_edge() {
    Fixture f;
    int64_t id = 0;
    {
        Database db(f.db_path);
        // Stored 200x100 with EXIF orientation 6, so upright it is 100x200.
        id = Fixture::insert(db, PHIG_SOURCE_DIR "/tests/data/orient_6.jpg", "2020-01-01T00:00:00Z",
                             std::string(64, 'd'), 1, "{}", "image");
    }
    phig::gtk::CollectionPtr coll = f.open();

    auto full = phig::gtk::image(*coll, id, 4096);
    g_assert_true(full.has_value());
    g_assert_cmpint(gdk_texture_get_width(full->get()), ==, 100);
    g_assert_cmpint(gdk_texture_get_height(full->get()), ==, 200);

    auto scaled = phig::gtk::image(*coll, id, 50);
    g_assert_true(scaled.has_value());
    g_assert_cmpint(gdk_texture_get_width(scaled->get()), ==, 25);
    g_assert_cmpint(gdk_texture_get_height(scaled->get()), ==, 50);
}

void test_an_image_names_a_missing_file_and_an_unknown_id() {
    Fixture f;
    phig::gtk::CollectionPtr coll = f.open();
    auto gone = phig::gtk::image(*coll, f.a, 512);
    g_assert_false(gone.has_value());
    g_assert_nonnull(strstr(gone.error().c_str(), "/nowhere/a.jpg"));
    g_assert_false(phig::gtk::image(*coll, 999999, 512).has_value());
}

// Runs an index of directory into catalog_dir to the end.
phig::gtk::IndexSummary index_to_end(const std::string& catalog_dir, const fs::path& directory) {
    auto run = phig::gtk::index_start(catalog_dir, {directory.string(), true, false});
    if (!run) g_error("index_start failed: %s", run.error().c_str());
    const gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (!phig::gtk::index_result(**run) && g_get_monotonic_time() < deadline) {
        phig::gtk::index_poll(**run);
        g_usleep(10000);
    }
    auto result = phig::gtk::index_result(**run);
    if (!result) g_error("index did not finish within 30s");
    if (!*result) g_error("index failed: %s", result->error().c_str());
    return **result;
}

// A directory holding copies of two sample photos.
fs::path photos_in(const fs::path& parent) {
    fs::path photos = parent / "photos";
    fs::create_directories(photos);
    fs::copy_file(PHIG_SOURCE_DIR "/tests/data/exif_sample.jpg", photos / "one.jpg");
    fs::copy_file(PHIG_SOURCE_DIR "/tests/data/orient_6.jpg", photos / "two.jpg");
    return photos;
}

void test_an_index_adds_new_files() {
    Fixture f;
    const fs::path photos = photos_in(f.dir);
    const phig::gtk::IndexSummary summary = index_to_end(f.dir.string(), photos);
    g_assert_true(summary.completed);
    g_assert_cmpuint(summary.total_files, ==, 2);
    g_assert_cmpuint(summary.processed, ==, 2);
    g_assert_cmpuint(summary.errors, ==, 0);

    phig::gtk::CollectionPtr coll = f.open();
    g_assert_cmpuint(ids_for(*coll, {}).size(), ==, 5);
    g_assert_cmpuint(ids_where(*coll, [](phig::gtk::Query& q) { q.search = "/photos/"; }).size(),
                     ==, 2);
}

void test_an_index_deletes_no_row_for_a_file_that_went_missing() {
    Fixture f;
    const fs::path photos = photos_in(f.dir);
    index_to_end(f.dir.string(), photos);
    fs::remove(photos / "two.jpg");

    const phig::gtk::IndexSummary again = index_to_end(f.dir.string(), photos);
    g_assert_cmpuint(again.total_files, ==, 1);

    phig::gtk::CollectionPtr coll = f.open();
    // The fixture's three rows, whose files never existed, and both photos.
    g_assert_cmpuint(ids_for(*coll, {}).size(), ==, 5);
    g_assert_cmpuint(
        ids_where(*coll, [](phig::gtk::Query& q) { q.search = "*/photos/two.jpg"; }).size(), ==, 1);
}

void test_an_index_creates_a_missing_catalog() {
    Fixture f;
    const fs::path photos = photos_in(f.dir);
    const fs::path catalog = f.dir / "new-catalog";
    g_assert_false(phig::gtk::open(catalog.string()).has_value());

    index_to_end(catalog.string(), photos);

    g_assert_true(fs::exists(catalog / "phig.db"));
    auto coll = phig::gtk::open(catalog.string());
    g_assert_true(coll.has_value());
    g_assert_cmpuint(ids_for(**coll, {}).size(), ==, 2);
}

void test_an_index_reports_progress_and_a_directory_it_cannot_walk() {
    Fixture f;
    const fs::path photos = photos_in(f.dir);
    auto run = phig::gtk::index_start(f.dir.string(), {photos.string(), true, false});
    g_assert_true(run.has_value());
    std::optional<phig::gtk::IndexProgress> last;
    const gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (!phig::gtk::index_result(**run) && g_get_monotonic_time() < deadline) {
        if (auto p = phig::gtk::index_poll(**run)) last = p;
        g_usleep(10000);
    }
    if (auto p = phig::gtk::index_poll(**run)) last = p;
    g_assert_true(last.has_value());
    g_assert_cmpuint(last->total, ==, 2);
    g_assert_cmpuint(last->done, ==, 2);

    auto bad = phig::gtk::index_start(f.dir.string(), {(f.dir / "absent").string(), true, false});
    g_assert_true(bad.has_value());
    const gint64 bad_deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    while (!phig::gtk::index_result(**bad) && g_get_monotonic_time() < bad_deadline)
        g_usleep(10000);
    if (!phig::gtk::index_result(**bad))
        g_error("index of an absent directory did not finish within 30s");
    g_assert_false(phig::gtk::index_result(**bad)->has_value());

    g_assert_false(phig::gtk::index_start(f.dir.string(), {"", true, false}).has_value());
    // An empty catalog directory would resolve to a path at the filesystem root.
    g_assert_false(phig::gtk::index_start("", {photos.string(), true, false}).has_value());
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
    g_test_add_func("/collection/upgrade-pending-is-false-for-a-current-catalog",
                    test_upgrade_pending_is_false_for_a_current_catalog);
    g_test_add_func("/collection/upgrade-pending-is-false-for-a-missing-catalog",
                    test_upgrade_pending_is_false_for_a_missing_catalog_and_creates_nothing);
    g_test_add_func("/collection/an-outdated-catalog-is-pending-until-open-upgrades-it",
                    test_an_outdated_catalog_is_pending_until_open_upgrades_it);
    g_test_add_func("/collection/open-of-a-current-catalog-has-no-backup",
                    test_open_of_a_current_catalog_has_no_backup);
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
    g_test_add_func("/collection/writes-change-exactly-the-named-images",
                    test_writes_change_exactly_the_named_images);
    g_test_add_func("/collection/gallery-set-replaces-the-gallery-an-image-had",
                    test_gallery_set_replaces_the_gallery_an_image_had);
    g_test_add_func("/collection/a-write-refuses-an-empty-name",
                    test_a_write_refuses_an_empty_name);
    g_test_add_func("/collection/a-write-is-busy-while-another-process-holds-the-writer-lock",
                    test_a_write_is_busy_while_another_process_holds_the_writer_lock);
    g_test_add_func("/collection/writes-started-together-in-this-process-both-land",
                    test_writes_started_together_in_this_process_both_land);
    g_test_add_func("/collection/a-write-skips-an-id-whose-row-was-deleted",
                    test_a_write_skips_an_id_whose_row_was_deleted);
    g_test_add_func("/collection/an-image-decodes-upright-within-its-max-edge",
                    test_an_image_decodes_upright_within_its_max_edge);
    g_test_add_func("/collection/an-image-names-a-missing-file-and-an-unknown-id",
                    test_an_image_names_a_missing_file_and_an_unknown_id);
    g_test_add_func("/collection/an-index-adds-new-files", test_an_index_adds_new_files);
    g_test_add_func("/collection/an-index-deletes-no-row-for-a-file-that-went-missing",
                    test_an_index_deletes_no_row_for_a_file_that_went_missing);
    g_test_add_func("/collection/an-index-creates-a-missing-catalog",
                    test_an_index_creates_a_missing_catalog);
    g_test_add_func("/collection/an-index-reports-progress-and-a-directory-it-cannot-walk",
                    test_an_index_reports_progress_and_a_directory_it_cannot_walk);
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
