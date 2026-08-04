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
     * It must return an object immediately rather than fetching synchronously. */
    GObject* item = g_list_model_get_item(G_LIST_MODEL(f->model), 0);
    g_assert_nonnull(item);
    g_assert_true(PHIG_IS_IMAGE_ROW(item));
    g_object_unref(item);
}

static void test_row_strings_survive_page_eviction(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* The ownership rule: a row must own copies, so it stays valid after its
     * page's phig_image_list is freed and after enough scrolling to evict it. */
    PhigImageRow* row =
        PHIG_IMAGE_ROW(g_list_model_get_item(G_LIST_MODEL(f->model), 0));
    g_assert_nonnull(row);
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

static void test_resident_pages_stay_bounded(Fixture* f, gconstpointer unused) {
    (void)unused;
    for (guint i = 0; i < (guint)f->total; i += 200)
        phig_gallery_model_fetch_page_sync(f->model, i / 200);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), <=,
                     phig_gallery_model_get_max_resident_pages(f->model));
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
#undef T
    return g_test_run();
}
