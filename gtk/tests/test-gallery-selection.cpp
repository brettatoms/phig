#include <glib.h>

#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

#include "connect.h"
#include "fake-collection.h"
#include "gallery-model.h"
#include "gallery-selection.h"

namespace {

// Five full pages of 200 and a short sixth of 50.
constexpr guint kRows = 1050;

struct Fixture {
    phig::gtk::Collection* coll;
    PhigGalleryModel* model;
    GtkSelectionModel* selection;
};

GtkSelectionModel* make_selection(PhigGalleryModel* model) {
    return GTK_SELECTION_MODEL(phig_gallery_selection_new(model));
}

void set_up(Fixture* f, gconstpointer) {
    f->coll = phig::gtk::fake::make(kRows).release();
    std::vector<int64_t> ids(kRows);
    std::iota(ids.begin(), ids.end(), 1);
    f->model = phig_gallery_model_new(*f->coll, std::move(ids));
    f->selection = make_selection(f->model);
}

// The model goes before the collection: its dispose waits for any page fetch
// still using it.
void tear_down(Fixture* f, gconstpointer) {
    g_clear_object(&f->selection);
    g_clear_object(&f->model);
    phig::gtk::CollectionClose{}(f->coll);
}

// The ids of the selected positions.
std::vector<int64_t> selected_ids(Fixture* f) {
    GtkBitset* set = gtk_selection_model_get_selection(f->selection);
    std::vector<int64_t> ids = phig_gallery_model_ids_in(f->model, set);
    gtk_bitset_unref(set);
    return ids;
}

std::vector<int64_t> id_range(int64_t first, guint n) {
    std::vector<int64_t> ids(n);
    std::iota(ids.begin(), ids.end(), first);
    return ids;
}

void assert_ids(const std::vector<int64_t>& got, const std::vector<int64_t>& want) {
    g_assert_cmpuint(got.size(), ==, want.size());
    for (size_t i = 0; i < want.size(); i++) g_assert_cmpint(got[i], ==, want[i]);
}

// Records every selection-changed as (position, n_items).
struct Changes {
    std::vector<std::pair<guint, guint>> seen;
};

void record_changes(Fixture* f, Changes& changes) {
    phig::gtk::connect<void(GtkSelectionModel*, guint, guint)>(
        f->selection, "selection-changed",
        [&changes](GtkSelectionModel*, guint position, guint n) noexcept {
            changes.seen.emplace_back(position, n);
        });
}

void assert_one_change(Changes& changes, guint position, guint n) {
    g_assert_cmpuint(changes.seen.size(), ==, 1);
    g_assert_cmpuint(changes.seen[0].first, ==, position);
    g_assert_cmpuint(changes.seen[0].second, ==, n);
    changes.seen.clear();
}

// Positions on a page that lands after they were selected stay selected: the
// page's real rows replace its placeholders at the same positions.
void test_a_selection_survives_its_page_landing(Fixture* f, gconstpointer) {
    gtk_selection_model_select_range(f->selection, 150, 100, FALSE);
    phig_gallery_model_fetch_page_sync(f->model, 1);
    phig_gallery_model_fetch_page_sync(f->model, 0);
    assert_ids(selected_ids(f), id_range(151, 100));
}

// A selected page that is evicted and fetched again comes back as new rows at
// the same positions, still selected.
void test_a_selection_survives_its_page_refetched(Fixture* f, gconstpointer) {
    phig_gallery_model_fetch_page_sync(f->model, 0);
    gtk_selection_model_select_range(f->selection, 0, 10, FALSE);
    for (guint page = 1; page <= 5; page++) phig_gallery_model_fetch_page_sync(f->model, page);
    phig_gallery_model_fetch_page_sync(f->model, 0);
    assert_ids(selected_ids(f), id_range(1, 10));
}

// Select all selects every position without asking for an item, so it
// fetches no page.
void test_select_all_fetches_nothing(Fixture* f, gconstpointer) {
    Changes changes;
    record_changes(f, changes);
    gtk_selection_model_select_all(f->selection);

    assert_one_change(changes, 0, kRows);
    assert_ids(selected_ids(f), id_range(1, kRows));
    g_assert_cmpuint(phig_gallery_model_pending_page_count(f->model), ==, 0);
    g_assert_cmpuint(phig_gallery_model_resident_page_count(f->model), ==, 0);
}

// selection-changed covers what changed, fires once per change, and does not
// fire when nothing changed.
void test_selection_changed_covers_what_changed(Fixture* f, gconstpointer) {
    Changes changes;
    record_changes(f, changes);

    gtk_selection_model_select_item(f->selection, 5, FALSE);
    assert_one_change(changes, 5, 1);
    gtk_selection_model_select_item(f->selection, 5, FALSE);
    g_assert_cmpuint(changes.seen.size(), ==, 0);

    gtk_selection_model_select_range(f->selection, 10, 20, FALSE);
    assert_one_change(changes, 10, 20);

    gtk_selection_model_unselect_item(f->selection, 12);
    assert_one_change(changes, 12, 1);

    // Replacing {5, 10-11, 13-29} with {7} changes 5 through 29.
    gtk_selection_model_select_item(f->selection, 7, TRUE);
    assert_one_change(changes, 5, 25);
    g_assert_true(gtk_selection_model_is_selected(f->selection, 7));
    g_assert_false(gtk_selection_model_is_selected(f->selection, 5));

    phig_gallery_model_fetch_page_sync(f->model, 0);
    g_assert_cmpuint(changes.seen.size(), ==, 0);

    gtk_selection_model_unselect_all(f->selection);
    assert_one_change(changes, 7, 1);
    gtk_selection_model_unselect_all(f->selection);
    g_assert_cmpuint(changes.seen.size(), ==, 0);
}

// set_selection, which rubber-band selection uses, changes only the masked
// positions.
void test_set_selection_changes_only_the_mask(Fixture* f, gconstpointer) {
    gtk_selection_model_select_item(f->selection, 5, FALSE);
    gtk_selection_model_select_item(f->selection, 20, FALSE);

    GtkBitset* selected = gtk_bitset_new_range(3, 2);
    GtkBitset* mask = gtk_bitset_new_range(0, 10);
    gtk_selection_model_set_selection(f->selection, selected, mask);
    gtk_bitset_unref(selected);
    gtk_bitset_unref(mask);

    assert_ids(selected_ids(f), {4, 5, 21});
}

// A page landing reaches the view through the selection unchanged.
void test_items_changed_is_forwarded(Fixture* f, gconstpointer) {
    std::vector<std::pair<guint, guint>> seen;
    phig::gtk::connect<void(GListModel*, guint, guint, guint)>(
        f->selection, "items-changed",
        [&seen](GListModel*, guint position, guint removed, guint added) noexcept {
            g_assert_cmpuint(removed, ==, added);
            seen.emplace_back(position, removed);
        });
    phig_gallery_model_fetch_page_sync(f->model, 5);

    g_assert_cmpuint(seen.size(), ==, 1);
    g_assert_cmpuint(seen[0].first, ==, 1000);
    g_assert_cmpuint(seen[0].second, ==, 50);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(f->selection)), ==, kRows);
}

// The grid's activate handler reads the gallery model through "model".
void test_model_property_is_the_gallery_model(Fixture* f, gconstpointer) {
    GListModel* model = nullptr;
    g_object_get(f->selection, "model", &model, nullptr);
    g_assert_true(model == G_LIST_MODEL(f->model));
    g_object_unref(model);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
#define T(name, fn) g_test_add("/gallery-selection/" name, Fixture, nullptr, set_up, fn, tear_down)
    T("survives-its-page-landing", test_a_selection_survives_its_page_landing);
    T("survives-its-page-refetched", test_a_selection_survives_its_page_refetched);
    T("select-all-fetches-nothing", test_select_all_fetches_nothing);
    T("selection-changed-covers-what-changed", test_selection_changed_covers_what_changed);
    T("set-selection-changes-only-the-mask", test_set_selection_changes_only_the_mask);
    T("items-changed-is-forwarded", test_items_changed_is_forwarded);
    T("model-property-is-the-gallery-model", test_model_property_is_the_gallery_model);
#undef T
    return g_test_run();
}
