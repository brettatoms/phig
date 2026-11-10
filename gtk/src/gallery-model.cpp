#include "gallery-model.h"

/* One page holds this many rows. Not varied in measurement (task-7-report.md):
 * page-fetch latency (0.85ms warm / 19ms cold per page fetch) is never
 * the bottleneck a full-range scroll hits, so there was no result to tune
 * this against. */
#define PHIG_PAGE_SIZE 200

/* Pages resident at once: the requested page plus this many either side,
 * rounded up to an odd budget so eviction has a clear least-recently-used
 * end. Measured at 1, 2, and 4 against the full 29,953-row scroll
 * (task-7-report.md): all three produce the same frame timings and
 * comparable RSS. The original explanation — that main-thread texture and
 * placeholder work dominates — was disproven on 2026-08-06: stubbing thumbnail
 * loading out entirely leaves frame timing unchanged, so page-fetch radius was
 * never going to move it either. 2 is kept as a starting point that measured no
 * worse than the alternatives. */
#define PHIG_PAGE_RADIUS 2
#define PHIG_MAX_RESIDENT_PAGES (2 * PHIG_PAGE_RADIUS + 1)

/* Cached per-position placeholders, capped independently of resident pages:
 * a scroll that outruns fetches touches positions across many more pages
 * than PHIG_MAX_RESIDENT_PAGES bounds, so this needs its own budget rather
 * than inheriting that one. At its current derived value (1000 with
 * PHIG_PAGE_RADIUS=2), a full-range fast-drag scroll — both warm and
 * miss-heavy — produced zero "Duplicate item detected" warnings
 * (task-7-report.md), so this stays derived rather than pinned to a
 * separately-measured constant. */
#define PHIG_MAX_PLACEHOLDERS (PHIG_MAX_RESIDENT_PAGES * PHIG_PAGE_SIZE)

struct _PhigImageRow {
    GObject parent_instance;
    gboolean placeholder;
    int64_t id;
    char* path;
    char* sha256;
    char* filename;
};

G_DEFINE_FINAL_TYPE(PhigImageRow, phig_image_row, G_TYPE_OBJECT)

static void phig_image_row_finalize(GObject* obj) {
    PhigImageRow* self = PHIG_IMAGE_ROW(obj);
    g_free(self->path);
    g_free(self->sha256);
    g_free(self->filename);
    G_OBJECT_CLASS(phig_image_row_parent_class)->finalize(obj);
}

static void phig_image_row_class_init(PhigImageRowClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = phig_image_row_finalize;
}

static void phig_image_row_init(PhigImageRow* self) {
    (void)self;
}

gboolean phig_image_row_is_placeholder(PhigImageRow* row) {
    return row->placeholder;
}

int64_t phig_image_row_get_id(PhigImageRow* row) {
    return row->id;
}

const char* phig_image_row_get_path(PhigImageRow* row) {
    return row->path;
}

const char* phig_image_row_get_sha256(PhigImageRow* row) {
    return row->sha256;
}

const char* phig_image_row_get_filename(PhigImageRow* row) {
    return row->filename;
}

static PhigImageRow* phig_image_row_new_placeholder(void) {
    PhigImageRow* row = PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, NULL));
    row->placeholder = TRUE;
    return row;
}

/* Copies every field it exposes out of data, whose strings belong to its page. */
static PhigImageRow* phig_image_row_new_from_data(const PhigRowData* data) {
    PhigImageRow* row = PHIG_IMAGE_ROW(g_object_new(PHIG_TYPE_IMAGE_ROW, NULL));
    row->placeholder = FALSE;
    row->id = data->id;
    row->path = g_strdup(data->path);
    row->sha256 = g_strdup(data->sha256);
    row->filename = g_strdup(data->filename);
    return row;
}

/* One resident page: an array of row refs plus bookkeeping for the LRU. */
typedef struct {
    guint page_index;
    PhigImageRow** rows; /* PHIG_PAGE_SIZE slots, only [0, n_rows) valid */
    guint n_rows;
    gint64 last_used; /* logical clock; higher is more recent */
} PhigPage;

static void phig_page_free(PhigPage* page) {
    for (guint i = 0; i < page->n_rows; i++)
        g_object_unref(page->rows[i]);
    g_free(page->rows);
    g_free(page);
}

/* One cached placeholder row for one non-resident position, with its own LRU
 * timestamp so the placeholder table can be capped independently of pages. */
typedef struct {
    PhigImageRow* row;
    gint64 last_used;
} PhigPlaceholderEntry;

static void phig_placeholder_entry_free(PhigPlaceholderEntry* entry) {
    g_object_unref(entry->row);
    g_free(entry);
}

struct _PhigGalleryModel {
    GObject parent_instance;
    PhigCollection* coll;
    guint total;
    GHashTable* pages; /* page_index (guint, via GUINT_TO_POINTER) -> PhigPage* */
    gint64 clock;
    GHashTable* pending; /* page_index being fetched -> TRUE, to avoid duplicate fetches */

    /* Placeholder rows for non-resident positions, one per position, keyed by
     * position (guint, via GUINT_TO_POINTER) -> PhigPlaceholderEntry*. GTK's
     * list-item manager keys recycled widgets by item pointer identity: a
     * single object shared across positions makes it pick a widget for the
     * wrong position at random (glib#3030), and a fresh object per call
     * breaks GtkSelectionModel's own identity tracking for repeated queries
     * of the same position. One cached object per position satisfies both.
     * Bounded the same way pages are: an LRU capped at
     * PHIG_MAX_PLACEHOLDERS, independent of pages so a fast scroll that
     * outruns fetches cannot grow it past that regardless of how many
     * distinct pages get requested. Entries for a position are also dropped
     * outright once that position's page lands (phig_gallery_model_install_page). */
    GHashTable* placeholders;
    gint64 placeholder_clock;

    /* coll is borrowed from the caller, not owned, so pinning self alive (the
     * GWeakRef in FetchTaskData) does not keep it alive. in_flight counts
     * worker threads currently between resolving that weak ref and dropping
     * it — i.e. actually inside phig_collection_page. dispose blocks until it
     * reaches zero, which is what makes the header's "caller must not close
     * coll while any reference to the model is outstanding" contract actually
     * true: the caller's final unref will not return while a worker is still
     * using it. */
    GMutex in_flight_lock;
    GCond in_flight_cond;
    guint in_flight;
};

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    PhigGalleryModel, phig_gallery_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, phig_gallery_model_list_model_iface_init))

/* Emitted on the main thread when a page fetch fails, after the affected
 * positions are left as placeholders (no items-changed for them, since
 * nothing changed). Carries no per-row detail: a GUI is expected to surface
 * this once regardless of how many pages fail, not once per row. */
enum { SIGNAL_PAGE_FETCH_FAILED, N_SIGNALS };
static guint signals[N_SIGNALS];

static GType phig_gallery_model_get_item_type(GListModel* list) {
    (void)list;
    return PHIG_TYPE_IMAGE_ROW;
}

static guint phig_gallery_model_get_n_items(GListModel* list) {
    return PHIG_GALLERY_MODEL(list)->total;
}

/* Builds owned rows from a freshly fetched page and frees it. Called on
 * whichever thread fetched the page (worker for async, the caller's for the
 * sync test helper); it touches no GTK or main-loop state. */
static PhigPage* phig_page_new_from_rows(guint page_index, PhigRowPage* fetched) {
    PhigPage* page = g_new0(PhigPage, 1);
    page->page_index = page_index;
    page->n_rows = fetched->n_rows;
    page->rows = g_new0(PhigImageRow*, page->n_rows);
    for (guint i = 0; i < page->n_rows; i++)
        page->rows[i] = phig_image_row_new_from_data(&fetched->rows[i]);
    phig_row_page_free(fetched);
    return page;
}

/* Evicts least-recently-used pages until resident count is within budget.
 * Rows already handed to callers survive: they are separately refcounted. */
static void phig_gallery_model_evict_if_needed(PhigGalleryModel* self) {
    while (g_hash_table_size(self->pages) > PHIG_MAX_RESIDENT_PAGES) {
        GHashTableIter iter;
        gpointer key, value;
        gpointer lru_key = NULL;
        PhigPage* lru_page = NULL;

        g_hash_table_iter_init(&iter, self->pages);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            PhigPage* page = static_cast<PhigPage*>(value);
            if (lru_page == NULL || page->last_used < lru_page->last_used) {
                lru_key = key;
                lru_page = page;
            }
        }
        g_hash_table_remove(self->pages, lru_key);
    }
}

/* Returns the cached placeholder for position, creating and caching one if
 * absent. Evicts the least-recently-used placeholder first if the table is
 * already at its cap, so a scroll that outruns fetches cannot grow this
 * table without bound regardless of how many distinct pages get touched. */
static PhigImageRow* phig_gallery_model_get_placeholder(PhigGalleryModel* self, guint position) {
    PhigPlaceholderEntry* entry =
        static_cast<PhigPlaceholderEntry*>(g_hash_table_lookup(self->placeholders, GUINT_TO_POINTER(position)));
    if (entry != NULL) {
        entry->last_used = ++self->placeholder_clock;
        return entry->row;
    }

    if (g_hash_table_size(self->placeholders) >= PHIG_MAX_PLACEHOLDERS) {
        GHashTableIter iter;
        gpointer key, value;
        gpointer lru_key = NULL;
        PhigPlaceholderEntry* lru_entry = NULL;

        g_hash_table_iter_init(&iter, self->placeholders);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            PhigPlaceholderEntry* candidate = static_cast<PhigPlaceholderEntry*>(value);
            if (lru_entry == NULL || candidate->last_used < lru_entry->last_used) {
                lru_key = key;
                lru_entry = candidate;
            }
        }
        g_hash_table_remove(self->placeholders, lru_key);
    }

    entry = g_new0(PhigPlaceholderEntry, 1);
    entry->row = phig_image_row_new_placeholder();
    entry->last_used = ++self->placeholder_clock;
    g_hash_table_insert(self->placeholders, GUINT_TO_POINTER(position), entry);
    return entry->row;
}

/* Drops cached placeholders for [position, position + n_rows): those
 * positions now have real rows, so nothing will ask get_item for a
 * placeholder there again until the page is evicted. */
static void phig_gallery_model_drop_placeholders(PhigGalleryModel* self, guint position,
                                                  guint n_rows) {
    for (guint i = 0; i < n_rows; i++)
        g_hash_table_remove(self->placeholders, GUINT_TO_POINTER(position + i));
}

/* Inserts a fetched page, marks it most-recently-used, evicts if over budget,
 * and emits items-changed for the range it covers. */
static void phig_gallery_model_install_page(PhigGalleryModel* self, PhigPage* page) {
    page->last_used = ++self->clock;
    guint position = page->page_index * PHIG_PAGE_SIZE;
    guint n_rows = page->n_rows;
    g_hash_table_insert(self->pages, GUINT_TO_POINTER(page->page_index), page);
    phig_gallery_model_evict_if_needed(self);
    phig_gallery_model_drop_placeholders(self, position, n_rows);
    /* removed == added == n_rows: an in-place swap of placeholders for real
     * rows at the same positions. The total item count never changes after
     * construction, so this is not an insertion or a deletion. GTK compares
     * this to a no-op unless removed/added are nonzero, so the view would
     * never re-query get_item for this range without it. */
    g_list_model_items_changed(G_LIST_MODEL(self), position, n_rows, n_rows);
}

static void phig_gallery_model_touch_page(PhigGalleryModel* self, PhigPage* page) {
    page->last_used = ++self->clock;
}

/* GTask state for the async fetch. model is a weak reference, not a strong
 * one: a GTask only releases its source_object/task_data once its completion
 * callback has been dispatched, which needs a main-loop iteration that a
 * caller destroying the model synchronously (as the sync test helper's
 * caller does, and as app shutdown may do) will never provide. A strong ref
 * here would then keep the model, and the collection it points at, alive past
 * when the caller expects to safely free them. Each thread-side use resolves
 * a temporary strong ref via g_weak_ref_get and drops it immediately after,
 * so the model cannot be finalized out from under a call in progress, but an
 * idle GTask callback that never runs cannot pin it forever either. */
typedef struct {
    GWeakRef model;
    guint page_index;
    PhigRowPage* fetched; /* set on the worker thread */
} FetchTaskData;

static void fetch_task_data_free(FetchTaskData* data) {
    /* Set once phig_page_new_from_rows has consumed it (fetch_page_done);
     * still non-NULL here means the result was fetched but never installed
     * (model gone before the callback ran, or the callback never dispatched),
     * so this is the only place left to free it. */
    if (data->fetched != NULL)
        phig_row_page_free(data->fetched);
    g_weak_ref_clear(&data->model);
    g_free(data);
}

/* Registers as in-flight, then resolves the weak ref. Registering first
 * closes the race dispose is guarding against: if this returns a non-NULL
 * self, dispose is guaranteed to block (in phig_gallery_model_dispose) until
 * phig_gallery_model_leave_in_flight is called, so coll cannot be closed
 * out from under the caller. If the model is already gone, unregisters
 * immediately and returns NULL. */
static PhigGalleryModel* phig_gallery_model_enter_in_flight(FetchTaskData* data) {
    PhigGalleryModel* self = static_cast<PhigGalleryModel*>(g_weak_ref_get(&data->model));
    if (self == NULL)
        return NULL;

    g_mutex_lock(&self->in_flight_lock);
    self->in_flight++;
    g_mutex_unlock(&self->in_flight_lock);
    return self;
}

static void phig_gallery_model_leave_in_flight(PhigGalleryModel* self) {
    g_mutex_lock(&self->in_flight_lock);
    self->in_flight--;
    g_cond_broadcast(&self->in_flight_cond);
    g_mutex_unlock(&self->in_flight_lock);
    g_object_unref(self);
}

static void fetch_page_thread_func(GTask* task, gpointer source_object, gpointer task_data,
                                    GCancellable* cancellable) {
    (void)source_object;
    (void)cancellable;
    FetchTaskData* data = static_cast<FetchTaskData*>(task_data);
    PhigGalleryModel* self = phig_gallery_model_enter_in_flight(data);
    if (self == NULL) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    PhigRowPage* fetched =
        phig_collection_page(self->coll, data->page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    phig_gallery_model_leave_in_flight(self);

    if (fetched == NULL) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "phig_collection_page failed");
        return;
    }
    data->fetched = fetched;
    g_task_return_boolean(task, TRUE);
}

static void fetch_page_done(GObject* source, GAsyncResult* result, gpointer user_data) {
    (void)source;
    (void)user_data;
    GTask* task = G_TASK(result);
    FetchTaskData* data = static_cast<FetchTaskData*>(g_task_get_task_data(task));
    PhigGalleryModel* self = static_cast<PhigGalleryModel*>(g_weak_ref_get(&data->model));
    if (self == NULL)
        return; /* model was destroyed before this fetch's result landed */

    GError* error = NULL;
    if (!g_task_propagate_boolean(task, &error)) {
        if (error) {
            g_warning("gallery model: page %u fetch failed: %s", data->page_index, error->message);
            g_error_free(error);
        }
        g_hash_table_remove(self->pending, GUINT_TO_POINTER(data->page_index));
        g_signal_emit(self, signals[SIGNAL_PAGE_FETCH_FAILED], 0);
        g_object_unref(self);
        return;
    }

    PhigPage* page = phig_page_new_from_rows(data->page_index, data->fetched);
    data->fetched = NULL; /* consumed: phig_page_new_from_rows freed it */
    g_hash_table_remove(self->pending, GUINT_TO_POINTER(data->page_index));
    phig_gallery_model_install_page(self, page);
    g_object_unref(self);
}

static void phig_gallery_model_schedule_fetch(PhigGalleryModel* self, guint page_index) {
    if (g_hash_table_contains(self->pending, GUINT_TO_POINTER(page_index)))
        return;
    g_hash_table_add(self->pending, GUINT_TO_POINTER(page_index));

    FetchTaskData* data = g_new0(FetchTaskData, 1);
    g_weak_ref_init(&data->model, self);
    data->page_index = page_index;

    /* No source_object: a GTask refs its source_object for the task's own
     * lifetime, which would recreate the strong-ref hazard the weak
     * reference above avoids. */
    GTask* task = g_task_new(NULL, NULL, fetch_page_done, NULL);
    g_task_set_task_data(task, data, (GDestroyNotify)fetch_task_data_free);
    g_task_run_in_thread(task, fetch_page_thread_func);
    g_object_unref(task);
}

static gpointer phig_gallery_model_get_item(GListModel* list, guint position) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(list);
    if (position >= self->total)
        return NULL;

    guint page_index = position / PHIG_PAGE_SIZE;
    guint offset_in_page = position % PHIG_PAGE_SIZE;

    PhigPage* page = static_cast<PhigPage*>(g_hash_table_lookup(self->pages, GUINT_TO_POINTER(page_index)));
    if (page != NULL) {
        if (offset_in_page >= page->n_rows)
            return NULL; /* last page, past its actual row count */
        phig_gallery_model_touch_page(self, page);
        return g_object_ref(page->rows[offset_in_page]);
    }

    phig_gallery_model_schedule_fetch(self, page_index);
    return g_object_ref(phig_gallery_model_get_placeholder(self, position));
}

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface) {
    iface->get_item_type = phig_gallery_model_get_item_type;
    iface->get_n_items = phig_gallery_model_get_n_items;
    iface->get_item = phig_gallery_model_get_item;
}

/* The caller may close coll as soon as its last reference to this model
 * is released (see phig_gallery_model_new's contract), so dispose must not
 * return while a worker thread is still between phig_gallery_model_enter_in_flight
 * and phig_gallery_model_leave_in_flight — i.e. still inside phig_collection_page.
 * This is what makes that contract actually hold: without it, a
 * worker resolving its weak ref just before the caller's final unref could
 * still be running phig_collection_page after the caller closes coll. */
static void phig_gallery_model_dispose(GObject* obj) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(obj);
    g_debug("phig_gallery_model_dispose: entering, in_flight=%u", self->in_flight);
    g_mutex_lock(&self->in_flight_lock);
    while (self->in_flight > 0)
        g_cond_wait(&self->in_flight_cond, &self->in_flight_lock);
    g_mutex_unlock(&self->in_flight_lock);
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->dispose(obj);
}

static void phig_gallery_model_finalize(GObject* obj) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(obj);
    g_hash_table_destroy(self->pages);
    g_hash_table_destroy(self->pending);
    g_hash_table_destroy(self->placeholders);
    g_mutex_clear(&self->in_flight_lock);
    g_cond_clear(&self->in_flight_cond);
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->finalize(obj);
}

static void phig_gallery_model_class_init(PhigGalleryModelClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = phig_gallery_model_dispose;
    G_OBJECT_CLASS(klass)->finalize = phig_gallery_model_finalize;

    signals[SIGNAL_PAGE_FETCH_FAILED] =
        g_signal_new("page-fetch-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
                     NULL, NULL, G_TYPE_NONE, 0);
}

static void phig_gallery_model_init(PhigGalleryModel* self) {
    self->pages =
        g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)phig_page_free);
    self->pending = g_hash_table_new(NULL, NULL);
    self->placeholders =
        g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)phig_placeholder_entry_free);
    g_mutex_init(&self->in_flight_lock);
    g_cond_init(&self->in_flight_cond);
}

PhigGalleryModel* phig_gallery_model_new(PhigCollection* coll) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL));
    self->coll = coll;
    guint total = 0;
    self->total = phig_collection_count(coll, &total) ? total : 0;
    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    PhigRowPage* fetched =
        phig_collection_page(self->coll, page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE);
    if (fetched == NULL)
        return;
    phig_gallery_model_install_page(self, phig_page_new_from_rows(page_index, fetched));
}

guint phig_gallery_model_resident_page_count(PhigGalleryModel* self) {
    return g_hash_table_size(self->pages);
}

guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self) {
    (void)self;
    return PHIG_MAX_RESIDENT_PAGES;
}
