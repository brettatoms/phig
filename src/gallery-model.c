#include "gallery-model.h"

/* One page holds this many rows. Task 7 measures whether this and the
 * resident-page budget below need to change. */
#define PHIG_PAGE_SIZE 200

/* Pages resident at once: the requested page plus this many either side,
 * rounded up to an odd budget so eviction has a clear least-recently-used
 * end. Task 7 measures this against real scrolling. */
#define PHIG_PAGE_RADIUS 2
#define PHIG_MAX_RESIDENT_PAGES (2 * PHIG_PAGE_RADIUS + 1)

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
    PhigImageRow* row = g_object_new(PHIG_TYPE_IMAGE_ROW, NULL);
    row->placeholder = TRUE;
    return row;
}

/* Copies every field it exposes out of img. img->* strings point into the
 * containing phig_image_list and must not be retained past this call. */
static PhigImageRow* phig_image_row_new_from_image(const phig_image* img) {
    PhigImageRow* row = g_object_new(PHIG_TYPE_IMAGE_ROW, NULL);
    row->placeholder = FALSE;
    row->id = img->id;
    row->path = g_strdup(img->path);
    row->sha256 = g_strdup(img->sha256);
    row->filename = g_strdup(img->filename);
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

struct _PhigGalleryModel {
    GObject parent_instance;
    phig_ctx* ctx;
    phig_query* query;
    guint total;
    GHashTable* pages; /* page_index (guint, via GUINT_TO_POINTER) -> PhigPage* */
    gint64 clock;
    GHashTable* pending; /* page_index being fetched -> TRUE, to avoid duplicate fetches */
};

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    PhigGalleryModel, phig_gallery_model, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, phig_gallery_model_list_model_iface_init))

static GType phig_gallery_model_get_item_type(GListModel* list) {
    (void)list;
    return PHIG_TYPE_IMAGE_ROW;
}

static guint phig_gallery_model_get_n_items(GListModel* list) {
    return PHIG_GALLERY_MODEL(list)->total;
}

/* Builds owned rows from a freshly fetched page and frees the list. Called
 * on whichever thread fetched the page (worker for async, caller's thread
 * for the sync test helper) — building rows only touches the phig_image
 * list, never GTK/GLib main-loop state, so it is safe off the main thread. */
static PhigPage* phig_page_new_from_list(guint page_index, phig_image_list* list) {
    PhigPage* page = g_new0(PhigPage, 1);
    page->page_index = page_index;
    page->n_rows = (guint)list->count;
    page->rows = g_new0(PhigImageRow*, page->n_rows);
    for (guint i = 0; i < page->n_rows; i++)
        page->rows[i] = phig_image_row_new_from_image(&list->items[i]);
    phig_image_list_free(list);
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
            PhigPage* page = value;
            if (lru_page == NULL || page->last_used < lru_page->last_used) {
                lru_key = key;
                lru_page = page;
            }
        }
        g_hash_table_remove(self->pages, lru_key);
    }
}

/* Inserts a fetched page, marks it most-recently-used, evicts if over budget,
 * and emits items-changed for the range it covers. */
static void phig_gallery_model_install_page(PhigGalleryModel* self, PhigPage* page) {
    page->last_used = ++self->clock;
    guint position = page->page_index * PHIG_PAGE_SIZE;
    g_hash_table_insert(self->pages, GUINT_TO_POINTER(page->page_index), page);
    phig_gallery_model_evict_if_needed(self);
    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 0);
}

static void phig_gallery_model_touch_page(PhigGalleryModel* self, PhigPage* page) {
    page->last_used = ++self->clock;
}

/* GTask state for the async fetch. model is a weak reference, not a strong
 * one: a GTask only releases its source_object/task_data once its completion
 * callback has been dispatched, which needs a main-loop iteration that a
 * caller destroying the model synchronously (as the sync test helper's
 * caller does, and as app shutdown may do) will never provide. A strong ref
 * here would then keep the model, and the ctx/query it points at, alive past
 * when the caller expects to safely free them. Each thread-side use resolves
 * a temporary strong ref via g_weak_ref_get and drops it immediately after,
 * so the model cannot be finalized out from under a call in progress, but an
 * idle GTask callback that never runs cannot pin it forever either. */
typedef struct {
    GWeakRef model;
    guint page_index;
    phig_image_list* fetched; /* set on the worker thread */
} FetchTaskData;

static void fetch_task_data_free(FetchTaskData* data) {
    g_weak_ref_clear(&data->model);
    g_free(data);
}

static void fetch_page_thread_func(GTask* task, gpointer source_object, gpointer task_data,
                                    GCancellable* cancellable) {
    (void)source_object;
    (void)cancellable;
    FetchTaskData* data = task_data;
    PhigGalleryModel* self = g_weak_ref_get(&data->model);
    if (self == NULL) {
        g_task_return_boolean(task, FALSE);
        return;
    }

    phig_image_list* list = NULL;
    phig_status status = phig_query_page(self->ctx, self->query,
                                          (int64_t)data->page_index * PHIG_PAGE_SIZE,
                                          PHIG_PAGE_SIZE, &list);
    g_object_unref(self);

    if (status != PHIG_OK) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "phig_query_page failed: %d",
                                 (int)status);
        return;
    }
    data->fetched = list;
    g_task_return_boolean(task, TRUE);
}

static void fetch_page_done(GObject* source, GAsyncResult* result, gpointer user_data) {
    (void)source;
    (void)user_data;
    GTask* task = G_TASK(result);
    FetchTaskData* data = g_task_get_task_data(task);
    PhigGalleryModel* self = g_weak_ref_get(&data->model);
    if (self == NULL)
        return; /* model was destroyed before this fetch's result landed */

    GError* error = NULL;
    if (!g_task_propagate_boolean(task, &error)) {
        if (error) {
            g_warning("gallery model: page %u fetch failed: %s", data->page_index, error->message);
            g_error_free(error);
        }
        g_hash_table_remove(self->pending, GUINT_TO_POINTER(data->page_index));
        g_object_unref(self);
        return;
    }

    PhigPage* page = phig_page_new_from_list(data->page_index, data->fetched);
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

    PhigPage* page = g_hash_table_lookup(self->pages, GUINT_TO_POINTER(page_index));
    if (page != NULL) {
        if (offset_in_page >= page->n_rows)
            return NULL; /* last page, past its actual row count */
        phig_gallery_model_touch_page(self, page);
        return g_object_ref(page->rows[offset_in_page]);
    }

    phig_gallery_model_schedule_fetch(self, page_index);
    return phig_image_row_new_placeholder();
}

static void phig_gallery_model_list_model_iface_init(GListModelInterface* iface) {
    iface->get_item_type = phig_gallery_model_get_item_type;
    iface->get_n_items = phig_gallery_model_get_n_items;
    iface->get_item = phig_gallery_model_get_item;
}

static void phig_gallery_model_finalize(GObject* obj) {
    PhigGalleryModel* self = PHIG_GALLERY_MODEL(obj);
    g_hash_table_destroy(self->pages);
    g_hash_table_destroy(self->pending);
    G_OBJECT_CLASS(phig_gallery_model_parent_class)->finalize(obj);
}

static void phig_gallery_model_class_init(PhigGalleryModelClass* klass) {
    G_OBJECT_CLASS(klass)->finalize = phig_gallery_model_finalize;
}

static void phig_gallery_model_init(PhigGalleryModel* self) {
    self->pages =
        g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)phig_page_free);
    self->pending = g_hash_table_new(NULL, NULL);
}

PhigGalleryModel* phig_gallery_model_new(phig_ctx* ctx, phig_query* query) {
    PhigGalleryModel* self = g_object_new(PHIG_TYPE_GALLERY_MODEL, NULL);
    self->ctx = ctx;
    self->query = query;

    int64_t total = 0;
    phig_status status = phig_query_count(ctx, query, &total);
    self->total = (status == PHIG_OK && total > 0) ? (guint)total : 0;

    return self;
}

void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index) {
    phig_image_list* list = NULL;
    phig_status status = phig_query_page(self->ctx, self->query,
                                          (int64_t)page_index * PHIG_PAGE_SIZE, PHIG_PAGE_SIZE,
                                          &list);
    if (status != PHIG_OK)
        return;
    PhigPage* page = phig_page_new_from_list(page_index, list);
    phig_gallery_model_install_page(self, page);
}

guint phig_gallery_model_resident_page_count(PhigGalleryModel* self) {
    return g_hash_table_size(self->pages);
}

guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self) {
    (void)self;
    return PHIG_MAX_RESIDENT_PAGES;
}
