#include "gallery-model.h"

#include <condition_variable>
#include <expected>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gref.h"

namespace {

/* One page holds this many rows. Not varied in measurement (task-7-report.md):
 * page-fetch latency (0.85ms warm / 19ms cold per page fetch) is never
 * the bottleneck a full-range scroll hits, so there was no result to tune
 * this against. */
constexpr guint PHIG_PAGE_SIZE = 200;

/* Pages resident at once: the requested page plus this many either side,
 * rounded up to an odd budget so eviction has a clear least-recently-used
 * end. Measured at 1, 2, and 4 against the full 29,953-row scroll
 * (task-7-report.md): all three produce the same frame timings and
 * comparable RSS. Stubbing thumbnail loading out entirely leaves frame timing
 * unchanged, so page-fetch radius was never going to move it either. 2 is kept
 * as a starting point that measured no worse than the alternatives. */
constexpr guint PHIG_PAGE_RADIUS = 2;
constexpr guint PHIG_MAX_RESIDENT_PAGES = 2 * PHIG_PAGE_RADIUS + 1;

/* Cached per-position placeholders, capped independently of resident pages:
 * a scroll that outruns fetches touches positions across many more pages
 * than PHIG_MAX_RESIDENT_PAGES bounds, so this needs its own budget rather
 * than inheriting that one. At its current derived value (1000 with
 * PHIG_PAGE_RADIUS=2), a full-range fast-drag scroll, both warm and
 * miss-heavy, produced zero "Duplicate item detected" warnings
 * (task-7-report.md), so this stays derived rather than pinned to a
 * separately-measured constant. */
constexpr guint PHIG_MAX_PLACEHOLDERS = PHIG_MAX_RESIDENT_PAGES * PHIG_PAGE_SIZE;

}  // namespace

struct _PhigImageRow {
    GObject parent_instance;
    bool placeholder;
    phig::gtk::Row row;  // constructed in phig_image_row_init, destroyed in finalize
};

G_DEFINE_FINAL_TYPE(PhigImageRow, phig_image_row, G_TYPE_OBJECT)

static void phig_image_row_finalize(GObject* obj) noexcept {
    PHIG_IMAGE_ROW(obj)->row.~Row();
    G_OBJECT_CLASS(phig_image_row_parent_class)->finalize(obj);
}

static void phig_image_row_class_init(PhigImageRowClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = phig_image_row_finalize;
}

static void phig_image_row_init(PhigImageRow* self) { new (&self->row) phig::gtk::Row(); }

bool phig_image_row_is_placeholder(PhigImageRow* row) { return row->placeholder; }

int64_t phig_image_row_get_id(PhigImageRow* row) { return row->row.id; }

const std::string& phig_image_row_get_path(PhigImageRow* row) { return row->row.path; }

const std::string& phig_image_row_get_sha256(PhigImageRow* row) { return row->row.sha256; }

const std::string& phig_image_row_get_filename(PhigImageRow* row) { return row->row.filename; }

namespace {

GRef<PhigImageRow> new_placeholder() {
    GRef<PhigImageRow> row = adopt(PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, nullptr)));
    row->placeholder = true;
    return row;
}

GRef<PhigImageRow> new_row(phig::gtk::Row data) {
    GRef<PhigImageRow> row = adopt(PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, nullptr)));
    row->placeholder = false;
    row->row = std::move(data);
    return row;
}

// One resident page. Rows already handed to callers survive its eviction: they
// are separately refcounted.
struct Page {
    std::vector<GRef<PhigImageRow>> rows;
    std::list<guint>::iterator lru;  // this page's node in Impl::page_lru
};

// The cached placeholder for one non-resident position. GTK's list-item
// manager keys recycled widgets by item pointer identity: a single object
// shared across positions makes it pick a widget for the wrong position at
// random (glib#3030), and a fresh object per call breaks GtkSelectionModel's
// own identity tracking for repeated queries of the same position. One cached
// object per position satisfies both.
struct Placeholder {
    GRef<PhigImageRow> row;
    std::list<guint>::iterator lru;  // this entry's node in Impl::placeholder_lru
};

// State shared by the model and every fetch task, so a worker needs no
// reference to the model. dispose sets disposed and waits for in_flight to reach
// zero, which is what makes the header's "caller must not close coll while any
// reference to the model is outstanding" contract true: the caller's final unref
// does not return while a worker is inside phig::gtk::page, and no worker enters
// once it has.
struct FetchShared {
    phig::gtk::Collection* coll = nullptr;  // borrowed; see phig_gallery_model_new
    std::mutex mutex;
    std::condition_variable cv;
    guint in_flight = 0;    // workers inside phig::gtk::page; guarded by mutex
    bool disposed = false;  // guarded by mutex
};

struct Impl {
    std::shared_ptr<FetchShared> shared = std::make_shared<FetchShared>();
    guint total = 0;

    std::unordered_map<guint, Page> pages;  // by page index
    std::list<guint> page_lru;              // front = least recently used
    std::unordered_set<guint> pending;      // page indices being fetched

    // Bounded like pages, but separately, at PHIG_MAX_PLACEHOLDERS, so a fast
    // scroll that outruns fetches cannot grow it however many distinct pages
    // it touches. A position's entry is also dropped once its page lands.
    std::unordered_map<guint, Placeholder> placeholders;  // by position
    std::list<guint> placeholder_lru;                     // front = least recently used
};

}  // namespace

struct _PhigGalleryModel {
    GObject parent_instance;
    Impl impl;  // constructed in phig_gallery_model_init, destroyed in finalize
};

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(PhigGalleryModel, phig_gallery_model, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                                    phig_gallery_model_list_model_iface_init))

/* Emitted on the main thread when a page fetch fails, after the affected
 * positions are left as placeholders. Carries the collection's reason. */
enum { SIGNAL_PAGE_FETCH_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

namespace {

void touch(std::list<guint>& lru, std::list<guint>::iterator node) {
    lru.splice(lru.end(), lru, node);
}

// Evicts least-recently-used pages until the resident count is within budget.
void evict_pages(Impl& impl) {
    while (impl.pages.size() > PHIG_MAX_RESIDENT_PAGES) {
        impl.pages.erase(impl.page_lru.front());
        impl.page_lru.pop_front();
    }
}

// The cached placeholder for position, created if absent. Evicts the least
// recently used placeholder first if the table is at its cap.
PhigImageRow* placeholder_for(Impl& impl, guint position) {
    if (auto it = impl.placeholders.find(position); it != impl.placeholders.end()) {
        touch(impl.placeholder_lru, it->second.lru);
        return it->second.row.get();
    }
    if (impl.placeholders.size() >= PHIG_MAX_PLACEHOLDERS) {
        impl.placeholders.erase(impl.placeholder_lru.front());
        impl.placeholder_lru.pop_front();
    }
    impl.placeholder_lru.push_back(position);
    auto [it, inserted] = impl.placeholders.emplace(
        position, Placeholder{new_placeholder(), std::prev(impl.placeholder_lru.end())});
    return it->second.row.get();
}

// Drops cached placeholders for [position, position + n_rows): those positions
// now have real rows.
void drop_placeholders(Impl& impl, guint position, guint n_rows) {
    for (guint i = 0; i < n_rows; i++) {
        auto it = impl.placeholders.find(position + i);
        if (it == impl.placeholders.end()) continue;
        impl.placeholder_lru.erase(it->second.lru);
        impl.placeholders.erase(it);
    }
}

// Installs a fetched page as the most recently used, evicts if over budget,
// and emits items-changed for the range it covers.
void install_page(PhigGalleryModel* self, guint page_index, std::vector<phig::gtk::Row> rows) {
    Impl& impl = self->impl;
    Page page;
    page.rows.reserve(rows.size());
    for (phig::gtk::Row& row : rows) page.rows.push_back(new_row(std::move(row)));
    const guint n_rows = static_cast<guint>(page.rows.size());
    const guint position = page_index * PHIG_PAGE_SIZE;

    if (auto old = impl.pages.find(page_index); old != impl.pages.end()) {
        impl.page_lru.erase(old->second.lru);
        impl.pages.erase(old);
    }
    impl.page_lru.push_back(page_index);
    page.lru = std::prev(impl.page_lru.end());
    impl.pages.emplace(page_index, std::move(page));
    evict_pages(impl);
    drop_placeholders(impl, position, n_rows);
    /* removed == added == n_rows: an in-place swap of placeholders for real
     * rows at the same positions. The total item count never changes after
     * construction, so this is not an insertion or a deletion. GTK treats
     * this as a no-op unless removed/added are nonzero, so the view would
     * never re-query get_item for this range without it. */
    g_list_model_items_changed(G_LIST_MODEL(self), position, n_rows, n_rows);
}

/* GTask state for the async fetch. model is a weak reference, not a strong
 * one: a GTask only releases its task data once its completion callback has
 * been dispatched, which needs a main-loop iteration that a caller destroying
 * the model synchronously never provides. A strong ref here would keep the model
 * alive past when the caller expects to free it. Only fetch_done, on the main
 * thread, resolves it. The worker uses shared, never the model. */
struct FetchTask {
    GWeakRef model;
    std::shared_ptr<FetchShared> shared;
    guint page_index = 0;
    // Set on the worker thread. Empty if the model was disposed before the fetch ran.
    std::optional<std::expected<std::vector<phig::gtk::Row>, std::string>> fetched;
};

void fetch_thread(GTask* task, gpointer source_object, gpointer task_data,
                  GCancellable* cancellable) noexcept {
    (void)source_object;
    (void)cancellable;
    auto& data = *static_cast<FetchTask*>(task_data);
    FetchShared& shared = *data.shared;
    {
        std::lock_guard lock(shared.mutex);
        if (shared.disposed) {
            g_task_return_boolean(task, FALSE);
            return;
        }
        shared.in_flight++;
    }
    data.fetched.emplace(
        phig::gtk::page(*shared.coll, data.page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE));
    {
        std::lock_guard lock(shared.mutex);
        shared.in_flight--;
    }
    shared.cv.notify_all();
    g_task_return_boolean(task, TRUE);
}

void fetch_done(GObject* source, GAsyncResult* result, gpointer user_data) noexcept {
    (void)source;
    (void)user_data;
    auto& data = *static_cast<FetchTask*>(g_task_get_task_data(G_TASK(result)));
    GRef<PhigGalleryModel> self =
        adopt(static_cast<PhigGalleryModel*>(g_weak_ref_get(&data.model)));
    if (!self) return;  // the model was destroyed before this fetch's result landed

    self->impl.pending.erase(data.page_index);
    if (!data.fetched) return;
    if (!*data.fetched) {
        const std::string& reason = data.fetched->error();
        g_warning("gallery model: page %u fetch failed: %s", data.page_index, reason.c_str());
        g_signal_emit(self.get(), signals[SIGNAL_PAGE_FETCH_FAILED], 0, reason.c_str());
        return;
    }
    install_page(self.get(), data.page_index, std::move(**data.fetched));
}

void schedule_fetch(PhigGalleryModel* self, guint page_index) {
    if (!self->impl.pending.insert(page_index).second) return;

    auto* data = new FetchTask{};
    g_weak_ref_init(&data->model, self);
    data->shared = self->impl.shared;
    data->page_index = page_index;

    /* No source_object: a GTask refs its source_object for the task's own
     * lifetime, which would recreate the strong-ref hazard the weak
     * reference avoids. */
    GTask* task = g_task_new(nullptr, nullptr, fetch_done, nullptr);
    g_task_set_task_data(task, data, [](gpointer p) noexcept {
        auto* t = static_cast<FetchTask*>(p);
        g_weak_ref_clear(&t->model);
        delete t;
    });
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}

GType get_item_type(GListModel* list) noexcept {
    (void)list;
    return PHIG_TYPE_IMAGE_ROW;
}

guint get_n_items(GListModel* list) noexcept { return PHIG_GALLERY_MODEL(list)->impl.total; }

gpointer get_item(GListModel* list, guint position) noexcept {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(list);
    Impl& impl = self->impl;
    if (position >= impl.total) return nullptr;

    const guint page_index = position / PHIG_PAGE_SIZE;
    const guint offset_in_page = position % PHIG_PAGE_SIZE;
    if (auto it = impl.pages.find(page_index); it != impl.pages.end()) {
        if (offset_in_page >= it->second.rows.size())
            return nullptr;  // last page, past its actual row count
        touch(impl.page_lru, it->second.lru);
        return g_object_ref(it->second.rows[offset_in_page].get());
    }

    schedule_fetch(self, page_index);
    return g_object_ref(placeholder_for(impl, position));
}

}  // namespace

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface) {
    iface->get_item_type = get_item_type;
    iface->get_n_items = get_n_items;
    iface->get_item = get_item;
}

/* The caller may close coll as soon as its last reference to this model is
 * released, so dispose must not return while a worker is still inside
 * phig::gtk::page. It runs on the thread that dropped the last reference. */
static void phig_gallery_model_dispose(GObject* obj) noexcept {
    FetchShared& shared = *PHIG_GALLERY_MODEL(obj)->impl.shared;
    {
        std::unique_lock lock(shared.mutex);
        shared.disposed = true;
        shared.cv.wait(lock, [&shared] { return shared.in_flight == 0; });
    }
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->dispose(obj);
}

static void phig_gallery_model_finalize(GObject* obj) noexcept {
    PHIG_GALLERY_MODEL(obj)->impl.~Impl();
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->finalize(obj);
}

static void phig_gallery_model_class_init(PhigGalleryModelClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = phig_gallery_model_dispose;
    G_OBJECT_CLASS(klass)->finalize = phig_gallery_model_finalize;

    signals[SIGNAL_PAGE_FETCH_FAILED] =
        g_signal_new("page-fetch-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                     nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void phig_gallery_model_init(PhigGalleryModel* self) { new (&self->impl) Impl(); }

PhigGalleryModel* phig_gallery_model_new(phig::gtk::Collection& coll) {
    auto* self = PHIG_GALLERY_MODEL(g_object_new(PHIG_TYPE_GALLERY_MODEL, nullptr));
    self->impl.shared->coll = &coll;
    if (auto total = phig::gtk::count(coll))
        self->impl.total = *total;
    else
        g_warning("gallery model: count failed: %s", total.error().c_str());
    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    auto rows =
        phig::gtk::page(*self->impl.shared->coll, page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    if (rows) install_page(self, page_index, std::move(*rows));
}

guint phig_gallery_model_resident_page_count(PhigGalleryModel* self) {
    return static_cast<guint>(self->impl.pages.size());
}

guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self) {
    (void)self;
    return PHIG_MAX_RESIDENT_PAGES;
}
