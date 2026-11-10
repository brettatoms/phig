#include <glib.h>

#include <string>

#include "fake-collection.h"
#include "gallery-model.h"

/* Five full pages of 200 and a short sixth of 50. */
#define N_ROWS 1050

typedef struct {
    phig::gtk::Collection* coll;
    PhigGalleryModel* model;
} Fixture;

static void fixture_set_up(Fixture* f, gconstpointer unused) {
    (void)unused;
    f->coll = phig::gtk::fake::make(N_ROWS).release();
    f->model = phig_gallery_model_new(*f->coll);
}

/* The model goes first: its dispose waits for any page fetch still using the
 * collection. */
static void fixture_tear_down(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_clear_object(&f->model);
    phig::gtk::CollectionClose{}(f->coll);
}

static void test_n_items_matches_count(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->model)), ==, N_ROWS);
}

static void test_out_of_range_returns_null(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS));
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS + 1000));
}

static void test_get_item_returns_a_row_without_blocking(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* A cold model has no resident page, so this takes the placeholder path. A
     * synchronous implementation would have installed the page by the time
     * get_item returns, so the page still being non-resident is what shows a
     * placeholder came back rather than a blocking fetch. */
    GObject* item = static_cast<GObject*>(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_assert_nonnull(item);
    g_assert_true(PHIG_IS_IMAGE_ROW(item));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(item)));
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
    g_object_unref(item);
}

static void test_rows_carry_the_collections_fields(Fixture* f, gconstpointer unused) {
    (void)unused;
    phig_gallery_model_fetch_page_sync(f->model, 1);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 250));
    g_assert_false(phig_image_row_is_placeholder(row));

    const std::string sha = phig::gtk::fake::sha(250);
    g_assert_cmpint(phig_image_row_get_id(row), ==, 251);
    g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha.c_str());
    g_assert_cmpstr(phig_image_row_get_path(row), ==, "/fake/250.jpg");
    g_assert_cmpstr(phig_image_row_get_filename(row), ==, "250.jpg");
    g_object_unref(row);
}

static void test_last_page_is_short(Fixture* f, gconstpointer unused) {
    (void)unused;
    phig_gallery_model_fetch_page_sync(f->model, 5);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), N_ROWS - 1));
    g_assert_false(phig_image_row_is_placeholder(row));
    g_assert_cmpint(phig_image_row_get_id(row), ==, N_ROWS);
    g_object_unref(row);
}

static void test_row_strings_survive_page_eviction(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* A row owns copies of its strings, so it stays valid after its page is
     * freed and evicted. */
    phig_gallery_model_fetch_page_sync(f->model, 0);
    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    char* sha_copy = g_strdup(phig_image_row_get_sha256(row));

    for (guint page = 0; page * 200 < N_ROWS; page++)
        phig_gallery_model_fetch_page_sync(f->model, page);

    g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha_copy);
    g_free(sha_copy);
    g_object_unref(row);
}

static void test_placeholders_are_unique_per_position(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* GTK's list-item manager keys recycled widgets by item pointer identity
     * (glib#3030): one placeholder shared across positions makes it pick a
     * widget for the wrong position. GtkSelectionModel separately requires the
     * same object for repeated queries of one position. Both must hold. */
    GObject* a1 = static_cast<GObject*>(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    GObject* b1 = static_cast<GObject*>(g_list_model_get_item(G_LIST_MODEL(f->model), 400));
    GObject* a2 = static_cast<GObject*>(g_list_model_get_item(G_LIST_MODEL(f->model), 0));

    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(a1)));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(b1)));
    g_assert_true(a1 != b1);
    g_assert_true(a1 == a2);

    g_object_unref(a1);
    g_object_unref(b1);
    g_object_unref(a2);
}

static void test_resident_pages_stay_bounded(Fixture* f, gconstpointer unused) {
    (void)unused;
    for (guint page = 0; page * 200 < N_ROWS; page++)
        phig_gallery_model_fetch_page_sync(f->model, page);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), <=,
                     phig_gallery_model_get_max_resident_pages(f->model));
}

typedef struct {
    GMainLoop* loop;
    gboolean fired;
    guint position;
    guint removed;
    guint added;
} ItemsChangedCapture;

static void on_items_changed(GListModel* model, guint position, guint removed, guint added,
                             gpointer user_data) {
    (void)model;
    ItemsChangedCapture* capture = static_cast<ItemsChangedCapture*>(user_data);
    capture->fired = TRUE;
    capture->position = position;
    capture->removed = removed;
    capture->added = added;
    g_main_loop_quit(capture->loop);
}

static gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

static void disarm_timeout(guint timeout_id) {
    if (g_main_context_find_source_by_id(NULL, timeout_id) != NULL)
        g_source_remove(timeout_id);
}

static void test_async_fetch_emits_items_changed(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* The real path: get_item schedules a GTask fetch and returns a
     * placeholder. The completion callback runs on the main loop and emits the
     * items-changed a GtkGridView needs to re-query the range. */
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    ItemsChangedCapture capture = {.loop = loop, .fired = FALSE, .position = 0, .removed = 0, .added = 0};
    gulong handler_id =
        g_signal_connect(f->model, "items-changed", G_CALLBACK(on_items_changed), &capture);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);

    g_object_unref(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_signal_handler_disconnect(f->model, handler_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.fired);
    g_assert_cmpuint(capture.position, ==, 0);
    g_assert_cmpuint(capture.removed, ==, 200);
    g_assert_cmpuint(capture.added, ==, 200);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 1);
}

typedef struct {
    GMainLoop* loop;
    gboolean fired;
    std::string reason;
} FailedCapture;

static void on_page_fetch_failed(PhigGalleryModel* model, const char* reason,
                                 gpointer user_data) {
    (void)model;
    FailedCapture* capture = static_cast<FailedCapture*>(user_data);
    capture->fired = TRUE;
    capture->reason = reason;
    g_main_loop_quit(capture->loop);
}

static void test_failed_fetch_emits_page_fetch_failed(Fixture* f, gconstpointer unused) {
    (void)unused;
    phig::gtk::fake::fail_pages(*f->coll, true);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    FailedCapture capture = {.loop = loop, .fired = FALSE, .reason = {}};
    gulong handler_id = g_signal_connect(f->model, "page-fetch-failed",
                                         G_CALLBACK(on_page_fetch_failed), &capture);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "gallery model: page 0 fetch failed: fake: page fetch failed");

    g_object_unref(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_signal_handler_disconnect(f->model, handler_id);
    g_main_loop_unref(loop);

    g_test_assert_expected_messages();
    g_assert_true(capture.fired);
    g_assert_cmpstr(capture.reason.c_str(), ==, phig::gtk::fake::kPageError);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, NULL);
#define T(name, fn) \
    g_test_add("/gallery-model/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)
    T("n-items-matches-count", test_n_items_matches_count);
    T("out-of-range-returns-null", test_out_of_range_returns_null);
    T("get-item-no-block", test_get_item_returns_a_row_without_blocking);
    T("rows-carry-the-collections-fields", test_rows_carry_the_collections_fields);
    T("last-page-is-short", test_last_page_is_short);
    T("row-strings-survive-eviction", test_row_strings_survive_page_eviction);
    T("resident-pages-bounded", test_resident_pages_stay_bounded);
    T("placeholders-unique-per-position", test_placeholders_are_unique_per_position);
    T("async-fetch-emits-items-changed", test_async_fetch_emits_items_changed);
    T("failed-fetch-emits-page-fetch-failed", test_failed_fetch_emits_page_fetch_failed);
#undef T
    return g_test_run();
}
