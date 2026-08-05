#include <glib.h>
#include <phig/phig.h>
#include "gallery-model.h"

typedef struct {
    phig_ctx* ctx;
    phig_query* query;
    PhigGalleryModel* model;
    int64_t total;
} Fixture;

static void fixture_set_up(Fixture* f, gconstpointer unused) {
    (void)unused;
    const char* db = g_getenv("PHIG_TEST_DB");
    g_assert_nonnull(db);
    phig_config cfg = {0};
    cfg.db_path = db;
    cfg.mode = PHIG_READ_ONLY;
    g_assert_cmpint(phig_init(&cfg, &f->ctx), ==, PHIG_OK);
    g_assert_cmpint(phig_query_new(f->ctx, &f->query), ==, PHIG_OK);
    phig_query_set_sort(f->query, PHIG_SORT_DATE, TRUE);
    g_assert_cmpint(phig_query_count(f->ctx, f->query, &f->total), ==, PHIG_OK);
    f->model = phig_gallery_model_new(f->ctx, f->query);
}

static void fixture_tear_down(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_clear_object(&f->model);
    phig_query_free(f->query);
    phig_shutdown(f->ctx);
}

static void test_n_items_matches_count(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->model)), ==,
                     (guint)f->total);
}

static void test_out_of_range_returns_null(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), (guint)f->total));
    g_assert_null(g_list_model_get_item(G_LIST_MODEL(f->model), (guint)f->total + 1000));
}

static void test_get_item_returns_a_row_without_blocking(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* A cold model has no resident page, so this exercises the placeholder path.
     * It must return an object immediately rather than fetching synchronously.
     * A synchronous implementation would have installed the page by the time
     * get_item returns, so asserting the page is still non-resident right
     * after the call is what actually distinguishes "returned a placeholder"
     * from "blocked and returned the real row". */
    GObject* item = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    g_assert_nonnull(item);
    g_assert_true(PHIG_IS_IMAGE_ROW(item));
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
    g_object_unref(item);
}

static void test_row_strings_survive_page_eviction(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* The ownership rule: a row must own copies, so it stays valid after its
     * page's phig_image_list is freed and after enough scrolling to evict it. */
    PhigImageRow* row =
        PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_assert_nonnull(row);
    g_object_unref(row); /* placeholder from the cold lookup above; not the one under test */
    phig_gallery_model_fetch_page_sync(f->model, 0);           /* test-only helper */
    row = PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    char* sha_copy = g_strdup(phig_image_row_get_sha256(row));

    /* Walk far enough to evict page 0 under any sane resident-window size. */
    for (guint i = 0; i < (guint)f->total; i += 200)
        phig_gallery_model_fetch_page_sync(f->model, i / 200);

    g_assert_cmpstr(phig_image_row_get_sha256(row), ==, sha_copy);
    g_free(sha_copy);
    g_object_unref(row);
}

static void test_placeholders_are_unique_per_position(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* GTK's list-item manager keys recycled widgets by item pointer identity
     * (glib#3030): one placeholder object shared across positions makes it
     * pick a widget for the wrong position at random, which is what shipped
     * as 223 "Duplicate item detected" warnings and an empty-looking grid.
     * GtkSelectionModel separately requires that repeated queries of the
     * SAME position return the same object, or selection breaks. Both must
     * hold at once: distinct positions get distinct placeholders, and asking
     * for one position twice gets the same placeholder both times. */
    GObject* a1 = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    GObject* b1 = g_list_model_get_item(G_LIST_MODEL(f->model), 400);
    GObject* a2 = g_list_model_get_item(G_LIST_MODEL(f->model), 0);

    g_assert_true(PHIG_IS_IMAGE_ROW(a1));
    g_assert_true(PHIG_IS_IMAGE_ROW(b1));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(a1)));
    g_assert_true(phig_image_row_is_placeholder(PHIG_IMAGE_ROW(b1)));

    g_assert_true(a1 != b1);  /* distinct positions: distinct objects */
    g_assert_true(a1 == a2);  /* same position asked twice: same object */

    g_object_unref(a1);
    g_object_unref(b1);
    g_object_unref(a2);
}

static void test_resident_pages_stay_bounded(Fixture* f, gconstpointer unused) {
    (void)unused;
    for (guint i = 0; i < (guint)f->total; i += 200)
        phig_gallery_model_fetch_page_sync(f->model, i / 200);
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
    ItemsChangedCapture* capture = user_data;
    capture->fired = TRUE;
    capture->position = position;
    capture->removed = removed;
    capture->added = added;
    g_main_loop_quit(capture->loop);
}

static gboolean on_timeout(gpointer user_data) {
    GMainLoop* loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void test_async_fetch_emits_items_changed(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* The real path: get_item schedules a GTask fetch on a worker thread and
     * returns a placeholder immediately. This drives an actual GMainLoop so
     * the GTask's completion callback (fetch_page_done) can be dispatched,
     * and asserts the exact items-changed arguments a GtkGridView relies on
     * to re-query get_item for the range that just became resident. A 5s
     * timeout guards against a hang if the callback never fires. */
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    ItemsChangedCapture capture = {.loop = loop, .fired = FALSE};
    gulong handler_id =
        g_signal_connect(f->model, "items-changed", G_CALLBACK(on_items_changed), &capture);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);

    GObject* placeholder = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    g_object_unref(placeholder);

    g_main_loop_run(loop);

    g_source_remove(timeout_id);
    g_signal_handler_disconnect(f->model, handler_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.fired);
    g_assert_cmpuint(capture.position, ==, 0);
    g_assert_cmpuint(capture.removed, ==, 200);
    g_assert_cmpuint(capture.added, ==, 200);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 1);
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, NULL);
    if (!g_getenv("PHIG_TEST_DB")) {
        g_printerr("PHIG_TEST_DB unset; skipping\n");
        return 77; /* automake/ctest skip convention */
    }
#define T(name, fn) \
    g_test_add("/gallery-model/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)
    T("n-items-matches-count", test_n_items_matches_count);
    T("out-of-range-returns-null", test_out_of_range_returns_null);
    T("get-item-no-block", test_get_item_returns_a_row_without_blocking);
    T("row-strings-survive-eviction", test_row_strings_survive_page_eviction);
    T("resident-pages-bounded", test_resident_pages_stay_bounded);
    T("placeholders-unique-per-position", test_placeholders_are_unique_per_position);
    T("async-fetch-emits-items-changed", test_async_fetch_emits_items_changed);
#undef T
    return g_test_run();
}
