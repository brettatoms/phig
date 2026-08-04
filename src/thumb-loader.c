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
 * loader is borrowed, not reffed on the request itself: a ref here would keep
 * the loader's own refcount above zero for as long as any request is
 * outstanding, so the app's final unref could never reach dispose and
 * workers would never be asked to stop. The ref that keeps the loader alive
 * for delivery lives on the idle source instead — see
 * thumb_loader_post_result — which is released whether or not the source is
 * ever dispatched, so it cannot recreate that cycle. */
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
    /* May run on a worker thread (a request cancelled after loading but
     * before delivery is freed from thumb_loader_worker, not the main
     * thread). Safe here: GdkTexture is a plain GObject with no GTK widget
     * state tied to a particular thread, so dropping the last ref off the
     * main thread does not violate GTK's main-thread-only rule the way
     * unreffing a widget would. */
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

    /* Diagnostic only: counts idle sources queued by a worker but not yet run
     * on the main thread. Nothing blocks on this — see thumb_loader_dispose
     * for why a wait here would be wrong — it exists so tests and debugging
     * can observe that outstanding deliveries eventually reach zero. */
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

/* Runs on the main thread via g_idle_add_full. Delivers the result. Does not
 * free req — thumb_loader_post_destroy does that, since GLib calls it exactly
 * once after this (dispatched or not), and freeing req here too would make
 * thumb_loader_post_destroy's later g_free a use-after-free on the very next
 * line: GLib holds the same user_data pointer to pass to both. */
static gboolean thumb_loader_deliver(gpointer user_data) {
    ThumbRequest* req = user_data;
    ThumbLoader* loader = req->loader;

    if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable))
        return G_SOURCE_REMOVE;

    if (req->result == THUMB_OK) {
        GdkTexture* cached = thumb_loader_cache_insert(loader, req->sha256, req->texture);
        req->texture = NULL; /* ownership moved into thumb_loader_cache_insert */
        req->cb(THUMB_OK, cached, req->user_data);
        g_object_unref(cached);
    } else {
        req->cb(req->result, NULL, req->user_data);
    }

    return G_SOURCE_REMOVE;
}

/* GDestroyNotify for the idle source posted below. Runs exactly once no
 * matter how the source's life ends: after thumb_loader_deliver returns, or
 * directly if the source is destroyed without ever being dispatched (e.g.
 * the main context is torn down first). This is the single place req is
 * freed — a texture loaded by the worker but never installed by
 * thumb_loader_deliver (cancelled before or after that call) is still
 * released via thumb_request_free, so no path here leaks it. It is also
 * where the ref taken in thumb_loader_post_result is given back, so the
 * loader cannot be finalized while a post is outstanding without needing
 * dispose to wait on or drive the main loop itself. */
static void thumb_loader_post_destroy(gpointer user_data) {
    ThumbRequest* req = user_data;
    ThumbLoader* loader = req->loader;
    thumb_request_free(req);
    thumb_loader_leave_idle(loader);
    g_object_unref(loader);
}

/* Posts req for delivery on the main thread. The idle source itself holds
 * the reference that keeps loader alive until delivery happens (or the
 * source is destroyed undelivered) — not req, which only borrows loader.
 * That is what lets dispose join workers and finalize proceed without
 * pumping the main context: the last outstanding post, whenever it runs or
 * is destroyed, drops the loader's last ref and the object is freed from
 * there, not from a dispose that has to wait around for it. */
static void thumb_loader_post_result(ThumbLoader* self, ThumbRequest* req) {
    thumb_loader_enter_idle(self);
    g_object_ref(self);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, thumb_loader_deliver, req, thumb_loader_post_destroy);
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

        thumb_loader_post_result(self, req);
    }

    return NULL;
}

/* Stops every worker before the loader can be finalized. ctx is borrowed (see
 * thumb_loader_new's contract), so this must return only once no worker is
 * still inside phig_thumb_lookup on it — g_thread_join guarantees exactly
 * that for each thread. One THUMB_LOADER_STOP per worker is enough to stop
 * all of them, since each worker consumes exactly one item before exiting.
 * Guarded on n_threads rather than on self->workers being non-NULL: with
 * n_threads == 0, g_new0(GThread*, 0) returns NULL (a zero-size allocation is
 * not distinct from "no array"), so a NULL check here would also skip
 * shutdown entirely for a loader with no workers to join in the first place —
 * which is fine to skip, but the queue drain below is not, and must not be
 * folded into the same guard.
 *
 * Joining workers only stops new requests from being popped and looked up;
 * it says nothing about requests already sitting in the queue (a shutdown
 * with more queued work than workers had time to drain) or about idle
 * sources already posted to the main context. The former are drained and
 * freed directly here — nothing else will ever pop them once no worker
 * remains. The latter are not waited on: each holds its own ref on self (see
 * thumb_loader_post_result), so the object this dispose belongs to is kept
 * alive by them for as long as any are outstanding, and g_object_unref simply
 * is not called again until the last one lets go. dispose has no need to
 * pump the main context to find out when that happens, and must not — that
 * would run arbitrary application callbacks (including this loader's own
 * deliveries, and completely unrelated idle/timeout sources) as a side
 * effect of the caller's g_object_unref, on a partially-torn-down object. */
static void thumb_loader_dispose(GObject* obj) {
    ThumbLoader* self = PHIG_THUMB_LOADER(obj);

    if (self->n_threads > 0) {
        for (guint i = 0; i < self->n_threads; i++)
            g_async_queue_push(self->queue, THUMB_LOADER_STOP);
        for (guint i = 0; i < self->n_threads; i++)
            g_thread_join(self->workers[i]);
        g_clear_pointer(&self->workers, g_free);
    }

    if (self->queue != NULL) {
        gpointer item;
        while ((item = g_async_queue_try_pop(self->queue)) != NULL) {
            if (item != THUMB_LOADER_STOP)
                thumb_request_free(item);
        }
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

guint thumb_loader_pending_idle_count(ThumbLoader* self) {
    g_mutex_lock(&self->idle_lock);
    guint count = self->pending_idle;
    g_mutex_unlock(&self->idle_lock);
    return count;
}
