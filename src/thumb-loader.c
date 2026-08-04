#include "thumb-loader.h"

#include <limits.h>

/* Pushed once per worker at shutdown. GAsyncQueue asserts its data is
 * non-NULL, so a real sentinel pointer stands in for "stop"; workers compare
 * against this address, never dereference it. */
static const int thumb_loader_stop_sentinel;
#define THUMB_LOADER_STOP ((gpointer)&thumb_loader_stop_sentinel)

/* One in-flight request, from thumb_loader_request until its callback has
 * been posted (or discarded uncalled). Owned by whichever of queue/worker/
 * idle currently holds it; freed exactly once, by whichever of those three
 * places is last to touch it.
 *
 * loader is borrowed, not reffed: a ref here would keep the loader's own
 * refcount above zero for as long as any request is outstanding, so the
 * app's final unref could never reach dispose and workers would never be
 * asked to stop. Instead, a worker registers with thumb_loader_enter_idle
 * before calling g_idle_add, and dispose blocks until every such
 * registration has a matching thumb_loader_leave_idle. */
typedef struct {
    ThumbLoader* loader;
    char sha256[65];
    GCancellable* cancellable; /* reffed, or NULL */
    ThumbCallback cb;
    gpointer user_data;

    /* Set by the worker before posting to the main thread. */
    ThumbResult result;
    GdkTexture* texture; /* owned by this struct until installed in the cache or freed */
} ThumbRequest;

static void thumb_request_free(ThumbRequest* req) {
    g_clear_object(&req->cancellable);
    g_clear_object(&req->texture);
    g_free(req);
}

/* One cached texture plus its LRU linkage. entries owns the GList node; the
 * hash table and the LRU list both index the same entries, neither owns the
 * texture exclusively — dropping an entry always goes through
 * thumb_cache_entry_free so the byte total and the texture ref stay in sync. */
typedef struct {
    char* sha256;
    GdkTexture* texture;
    gsize bytes;
    GList* lru_link; /* node in loader->lru, ->data == this entry */
} ThumbCacheEntry;

struct _ThumbLoader {
    GObject parent_instance;
    phig_ctx* ctx;
    gsize cache_bytes_budget;

    GAsyncQueue* queue; /* ThumbRequest*, or THUMB_LOADER_STOP as a per-worker shutdown sentinel */
    GThread** workers;
    guint n_threads;

    GMutex cache_lock;
    GHashTable* cache;   /* sha256 -> ThumbCacheEntry*, owns the entry */
    GQueue lru;          /* front = least recently used; holds ThumbCacheEntry*, not owned here */
    gsize cache_bytes;

    /* Counts idle sources queued by a worker but not yet run on the main
     * thread. dispose joins every worker before it returns, which guarantees
     * no *new* g_idle_add call will happen, but says nothing about idle
     * sources already queued — those run on a later main-loop iteration,
     * which happens after a synchronous dispose/finalize would already have
     * freed self. dispose drives the main context itself (see
     * thumb_loader_dispose) until this reaches zero, closing that window. */
    GMutex idle_lock;
    guint pending_idle;
};

G_DEFINE_FINAL_TYPE(ThumbLoader, thumb_loader, G_TYPE_OBJECT)

static void thumb_cache_entry_free(ThumbCacheEntry* entry) {
    g_free(entry->sha256);
    g_object_unref(entry->texture);
    g_free(entry);
}

/* Caller must hold cache_lock. */
static void thumb_loader_evict_locked(ThumbLoader* self) {
    while (self->cache_bytes > self->cache_bytes_budget && !g_queue_is_empty(&self->lru)) {
        ThumbCacheEntry* victim = g_queue_pop_head(&self->lru);
        self->cache_bytes -= victim->bytes;
        g_hash_table_remove(self->cache, victim->sha256); /* frees victim via GDestroyNotify */
    }
}

/* Inserts a freshly loaded texture into the cache, taking ownership of the
 * reference passed in. Returns a new ref for the caller to deliver, since the
 * cache's own ref may be evicted (and dropped) before the callback runs. */
static GdkTexture* thumb_loader_cache_insert(ThumbLoader* self, const char* sha256,
                                              GdkTexture* texture) {
    g_mutex_lock(&self->cache_lock);

    ThumbCacheEntry* existing = g_hash_table_lookup(self->cache, sha256);
    if (existing != NULL) {
        /* Already cached by a concurrent request for the same sha; keep the
         * existing entry, drop the one just loaded, and just touch the LRU. */
        g_object_unref(texture);
        g_queue_unlink(&self->lru, existing->lru_link);
        g_queue_push_tail_link(&self->lru, existing->lru_link);
        GdkTexture* ref = g_object_ref(existing->texture);
        g_mutex_unlock(&self->cache_lock);
        return ref;
    }

    ThumbCacheEntry* entry = g_new0(ThumbCacheEntry, 1);
    entry->sha256 = g_strdup(sha256);
    entry->texture = texture; /* takes ownership of the ref passed in */
    entry->bytes = (gsize)gdk_texture_get_width(texture) * (gsize)gdk_texture_get_height(texture) * 4;

    g_hash_table_insert(self->cache, entry->sha256, entry);
    g_queue_push_tail(&self->lru, entry);
    entry->lru_link = self->lru.tail;
    self->cache_bytes += entry->bytes;

    thumb_loader_evict_locked(self);

    GdkTexture* ref = g_object_ref(entry->texture);
    g_mutex_unlock(&self->cache_lock);
    return ref;
}

/* Registers a pending idle post. Called on the worker thread before
 * g_idle_add, so dispose (called concurrently, on the main thread) either
 * observes the count before it is incremented — and blocks below until the
 * matching thumb_loader_leave_idle — or observes it already incremented,
 * which is the same outcome. Either way dispose cannot return while this
 * request's idle source might still run. */
static void thumb_loader_enter_idle(ThumbLoader* self) {
    g_mutex_lock(&self->idle_lock);
    self->pending_idle++;
    g_mutex_unlock(&self->idle_lock);
}

static void thumb_loader_leave_idle(ThumbLoader* self) {
    g_mutex_lock(&self->idle_lock);
    self->pending_idle--;
    g_mutex_unlock(&self->idle_lock);
}

/* Runs on the main thread via g_idle_add. Delivers the result and frees the
 * request. A texture loaded by the worker but never installed here (request
 * cancelled between load and this call) must still be released — req owns it
 * via thumb_request_free until it is handed off, so no path here leaks it. */
static gboolean thumb_loader_deliver(gpointer user_data) {
    ThumbRequest* req = user_data;
    ThumbLoader* loader = req->loader;

    if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable)) {
        thumb_request_free(req);
        thumb_loader_leave_idle(loader);
        return G_SOURCE_REMOVE;
    }

    if (req->result == THUMB_OK) {
        GdkTexture* cached = thumb_loader_cache_insert(loader, req->sha256, req->texture);
        req->texture = NULL; /* ownership moved into thumb_loader_cache_insert */
        req->cb(THUMB_OK, cached, req->user_data);
        g_object_unref(cached);
    } else {
        req->cb(req->result, NULL, req->user_data);
    }

    thumb_request_free(req);
    thumb_loader_leave_idle(loader);
    return G_SOURCE_REMOVE;
}

static gpointer thumb_loader_worker(gpointer data) {
    ThumbLoader* self = data;

    for (;;) {
        gpointer item = g_async_queue_pop(self->queue);
        if (item == THUMB_LOADER_STOP)
            break;
        ThumbRequest* req = item;

        if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable)) {
            thumb_request_free(req);
            continue;
        }

        char path[PATH_MAX];
        bool exists = false;
        phig_status status = phig_thumb_lookup(self->ctx, req->sha256, path, sizeof(path), &exists);

        if (status != PHIG_OK) {
            /* out_len is sized generously above; any failure here is a real
             * lookup error, not a too-small buffer. */
            req->result = THUMB_FAILED;
        } else if (!exists) {
            req->result = THUMB_MISS;
        } else {
            GError* error = NULL;
            GdkTexture* texture = gdk_texture_new_from_filename(path, &error);
            if (texture == NULL) {
                g_warning("thumb loader: failed to load %s: %s", path,
                          error ? error->message : "unknown error");
                g_clear_error(&error);
                req->result = THUMB_FAILED;
            } else {
                req->result = THUMB_OK;
                req->texture = texture;
            }
        }

        if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable)) {
            thumb_request_free(req);
            continue;
        }

        thumb_loader_enter_idle(self);
        g_idle_add(thumb_loader_deliver, req);
    }

    return NULL;
}

/* Stops every worker before the loader can be finalized. ctx is borrowed (see
 * thumb_loader_new's contract), so this must return only once no worker is
 * still inside phig_thumb_lookup on it — g_thread_join guarantees exactly
 * that for each thread. One THUMB_LOADER_STOP per worker is enough to stop
 * all of them, since each worker consumes exactly one item before exiting.
 *
 * Joining workers only guarantees no *new* idle source will be queued; idle
 * sources already queued still need a main-loop iteration to actually run
 * thumb_loader_deliver. dispose runs on the main thread, so it cannot just
 * g_cond_wait for pending_idle to drop to zero — nothing would ever dispatch
 * those idle sources while this thread is blocked. Instead it drives the
 * default main context itself until the count clears. */
static void thumb_loader_dispose(GObject* obj) {
    ThumbLoader* self = PHIG_THUMB_LOADER(obj);

    if (self->workers != NULL) {
        for (guint i = 0; i < self->n_threads; i++)
            g_async_queue_push(self->queue, THUMB_LOADER_STOP);
        for (guint i = 0; i < self->n_threads; i++)
            g_thread_join(self->workers[i]);
        g_clear_pointer(&self->workers, g_free);

        g_mutex_lock(&self->idle_lock);
        while (self->pending_idle > 0) {
            g_mutex_unlock(&self->idle_lock);
            g_main_context_iteration(NULL, TRUE);
            g_mutex_lock(&self->idle_lock);
        }
        g_mutex_unlock(&self->idle_lock);
    }

    G_OBJECT_CLASS(thumb_loader_parent_class)->dispose(obj);
}

static void thumb_loader_finalize(GObject* obj) {
    ThumbLoader* self = PHIG_THUMB_LOADER(obj);
    g_hash_table_destroy(self->cache);
    g_queue_clear(&self->lru);
    g_mutex_clear(&self->cache_lock);
    g_mutex_clear(&self->idle_lock);
    if (self->queue != NULL)
        g_async_queue_unref(self->queue);
    g_free(self->workers);
    G_OBJECT_CLASS(thumb_loader_parent_class)->finalize(obj);
}

static void thumb_loader_class_init(ThumbLoaderClass* klass) {
    G_OBJECT_CLASS(klass)->dispose = thumb_loader_dispose;
    G_OBJECT_CLASS(klass)->finalize = thumb_loader_finalize;
}

static void thumb_loader_init(ThumbLoader* self) {
    g_mutex_init(&self->cache_lock);
    self->cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                         (GDestroyNotify)thumb_cache_entry_free);
    g_queue_init(&self->lru);
    g_mutex_init(&self->idle_lock);
}

ThumbLoader* thumb_loader_new(phig_ctx* ctx, guint n_threads, gsize cache_bytes) {
    ThumbLoader* self = g_object_new(PHIG_TYPE_THUMB_LOADER, NULL);
    self->ctx = ctx;
    self->cache_bytes_budget = cache_bytes;
    self->n_threads = n_threads;
    self->queue = g_async_queue_new();
    self->workers = g_new0(GThread*, n_threads);
    for (guint i = 0; i < n_threads; i++)
        self->workers[i] = g_thread_new("phig-thumb-loader", thumb_loader_worker, self);
    return self;
}

void thumb_loader_request(ThumbLoader* self, const char* sha256, guint32 max_dim,
                           GCancellable* cancellable, ThumbCallback cb, gpointer user_data) {
    (void)max_dim; /* M0's backend serves one fixed cache size; see header. */

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
        return; /* never dispatched, so cb must not run: matches the header's contract */

    ThumbRequest* req = g_new0(ThumbRequest, 1);
    req->loader = self;
    g_strlcpy(req->sha256, sha256, sizeof(req->sha256));
    req->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
    req->cb = cb;
    req->user_data = user_data;

    g_async_queue_push(self->queue, req);
}

gsize thumb_loader_cache_bytes(ThumbLoader* self) {
    g_mutex_lock(&self->cache_lock);
    gsize bytes = self->cache_bytes;
    g_mutex_unlock(&self->cache_lock);
    return bytes;
}

gint thumb_loader_queue_length(ThumbLoader* self) {
    return g_async_queue_length(self->queue);
}
