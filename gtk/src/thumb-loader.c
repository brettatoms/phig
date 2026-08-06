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
    int64_t image_id;
    char sha256[65];
    guint32 max_dim; /* 0 = load the cache's native size unscaled */
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
 * thumb_cache_entry_free so the byte total and the texture ref stay in sync.
 *
 * Keyed by "<sha256>@<size>", not sha256 alone: the same image is cached at
 * whatever sizes the grid has asked for, so a zoom that changes the cell size
 * cannot be served a stale texture built for the previous size. `key` owns the
 * string; `size` is kept for readability and eviction decisions. */
typedef struct {
    char* key;
    char sha256[65];
    guint32 size;
    GdkTexture* texture;
    gsize bytes;
    GList* lru_link; /* node in loader->lru, ->data == this entry */
} ThumbCacheEntry;

/* Cache key for one (image, requested size) pair. Size 0 means "the cache's
 * native size, unscaled" and gets its own key rather than aliasing any
 * particular pixel size. */
static char* thumb_cache_key(const char* sha256, guint32 size) {
    return g_strdup_printf("%s@%u", sha256, size);
}

struct _ThumbLoader {
    GObject parent_instance;
    phig_ctx* ctx;
    gsize cache_bytes_budget;

    GAsyncQueue* queue; /* ThumbRequest*, or THUMB_LOADER_STOP as a per-worker shutdown sentinel */
    GThread** workers;
    guint n_threads;

    GMutex cache_lock;
    GHashTable* cache;   /* "<sha256>@<size>" -> ThumbCacheEntry*, owns the entry */
    GQueue lru;          /* front = least recently used; holds ThumbCacheEntry*, not owned here */
    gsize cache_bytes;

    /* Diagnostic only: counts idle sources queued by a worker but not yet run
     * on the main thread. Nothing blocks on this — see thumb_loader_dispose
     * for why a wait here would be wrong — it exists so tests and debugging
     * can observe that outstanding deliveries eventually reach zero. */
    GMutex idle_lock;
    guint pending_idle;

    /* Set once thumb_loader_shutdown has joined every worker. Checked by
     * thumb_loader_deliver so an idle source already queued before shutdown
     * (holding only a loader ref, per thumb_loader_post_result) delivers
     * nothing instead of calling back into a caller that may itself be
     * mid-teardown. */
    gboolean stopped;
};

G_DEFINE_FINAL_TYPE(ThumbLoader, thumb_loader, G_TYPE_OBJECT)

static void thumb_cache_entry_free(ThumbCacheEntry* entry) {
    g_free(entry->key);
    g_object_unref(entry->texture);
    g_free(entry);
}

/* Caller must hold cache_lock. */
static void thumb_loader_evict_locked(ThumbLoader* self) {
    while (self->cache_bytes > self->cache_bytes_budget && !g_queue_is_empty(&self->lru)) {
        ThumbCacheEntry* victim = g_queue_pop_head(&self->lru);
        self->cache_bytes -= victim->bytes;
        g_hash_table_remove(self->cache, victim->key); /* frees victim via GDestroyNotify */
    }
}

/* Inserts a freshly loaded texture into the cache, taking ownership of the
 * reference passed in. Returns a new ref for the caller to deliver, since the
 * cache's own ref may be evicted (and dropped) before the callback runs. */
static GdkTexture* thumb_loader_cache_insert(ThumbLoader* self, const char* sha256, guint32 size,
                                              GdkTexture* texture) {
    g_mutex_lock(&self->cache_lock);

    char* key = thumb_cache_key(sha256, size);
    ThumbCacheEntry* existing = g_hash_table_lookup(self->cache, key);
    if (existing != NULL) {
        /* Already cached by a concurrent request for the same sha and size;
         * keep the existing entry, drop the one just loaded, touch the LRU. */
        g_free(key);
        g_object_unref(texture);
        g_queue_unlink(&self->lru, existing->lru_link);
        g_queue_push_tail_link(&self->lru, existing->lru_link);
        GdkTexture* ref = g_object_ref(existing->texture);
        g_mutex_unlock(&self->cache_lock);
        return ref;
    }

    ThumbCacheEntry* entry = g_new0(ThumbCacheEntry, 1);
    entry->key = key; /* takes ownership */
    g_strlcpy(entry->sha256, sha256, sizeof(entry->sha256));
    entry->size = size;
    entry->texture = texture; /* takes ownership of the ref passed in */
    entry->bytes = (gsize)gdk_texture_get_width(texture) * (gsize)gdk_texture_get_height(texture) * 4;

    g_hash_table_insert(self->cache, entry->key, entry);
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

    if (loader->stopped)
        return G_SOURCE_REMOVE;

    if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable))
        return G_SOURCE_REMOVE;

    if (req->result == THUMB_OK) {
        GdkTexture* cached =
            thumb_loader_cache_insert(loader, req->sha256, req->max_dim, req->texture);
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

        /* A row with no sha256 (not yet hashed by phig) has nothing to look
         * up or generate from; that is not a failure, just data phig has not
         * produced yet, so it is reported the same as a not-yet-generated
         * cache miss rather than counted as THUMB_FAILED. */
        if (req->sha256[0] == '\0') {
            req->result = THUMB_MISS;
            thumb_loader_post_result(self, req);
            continue;
        }

        char path[PATH_MAX];
        bool exists = false;
        phig_status status = phig_thumb_lookup(self->ctx, req->sha256, path, sizeof(path), &exists);
        bool generation_failed = false;

        if (status == PHIG_OK && !exists) {
            /* Generation is far slower than a lookup or a file load (hundreds
             * of milliseconds vs. about one), so by the time a queued request
             * gets here the user may have scrolled past it — check
             * cancellation immediately before paying that cost rather than
             * after. */
            if (req->cancellable != NULL && g_cancellable_is_cancelled(req->cancellable)) {
                thumb_request_free(req);
                continue;
            }
            if (phig_thumb_generate(self->ctx, req->image_id) == PHIG_OK)
                status = phig_thumb_lookup(self->ctx, req->sha256, path, sizeof(path), &exists);
            else
                generation_failed = true;
        }

        if (status != PHIG_OK || generation_failed) {
            /* out_len is sized generously above; any lookup failure here is a
             * real error, not a too-small buffer. A failed generation is
             * equally a real failure — the row has a sha256 but phig could
             * not decode the image — so both count as THUMB_FAILED. */
            req->result = THUMB_FAILED;
        } else if (!exists) {
            req->result = THUMB_MISS;
        } else {
            GError* error = NULL;
            GdkTexture* texture = NULL;
            /* Decode straight to the requested display size when the caller
             * asked for one smaller than the cache's 512px: the scale is paid
             * once per (sha, size) here on a worker rather than by the GPU on
             * every frame, and the resident texture shrinks with the square of
             * the size, so the byte budget holds proportionally more of them. */
            if (req->max_dim > 0) {
                /* Decode straight to the requested size: scaling here costs one
                 * pass per (sha, size) on a worker instead of leaving the GPU to
                 * downscale a 512px texture into a smaller cell, and the smaller
                 * texture is what makes the byte budget hold proportionally more
                 * of them. gdk_texture_downscale does not exist in GTK 4.22, and
                 * gdk_texture_new_for_pixbuf is deprecated, so go via
                 * GdkMemoryTexture over the pixbuf's own buffer. */
                GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_size(
                    path, (int)req->max_dim, (int)req->max_dim, &error);
                if (pixbuf != NULL) {
                    GBytes* bytes = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                                                (gsize)gdk_pixbuf_get_rowstride(pixbuf) *
                                                    (gsize)gdk_pixbuf_get_height(pixbuf));
                    texture = GDK_TEXTURE(gdk_memory_texture_new(
                        gdk_pixbuf_get_width(pixbuf), gdk_pixbuf_get_height(pixbuf),
                        gdk_pixbuf_get_has_alpha(pixbuf) ? GDK_MEMORY_R8G8B8A8
                                                         : GDK_MEMORY_R8G8B8,
                        bytes, (gsize)gdk_pixbuf_get_rowstride(pixbuf)));
                    g_bytes_unref(bytes);
                    g_object_unref(pixbuf);
                }
            } else {
                texture = gdk_texture_new_from_filename(path, &error);
            }
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

/* Stops every worker so the loader's ctx (borrowed; see thumb_loader_new's
 * contract) can be freed safely afterward. Returning only once no worker is
 * still inside phig_thumb_lookup/phig_thumb_generate on it is g_thread_join's
 * guarantee for each thread. One THUMB_LOADER_STOP per worker is enough to
 * stop all of them, since each worker consumes exactly one item before
 * exiting. Guarded on n_threads rather than on self->workers being non-NULL:
 * with n_threads == 0, g_new0(GThread*, 0) returns NULL (a zero-size
 * allocation is not distinct from "no array"), so a NULL check here would
 * also skip shutdown entirely for a loader with no workers to join in the
 * first place — which is fine to skip, but the queue drain below is not, and
 * must not be folded into the same guard.
 *
 * Joining workers only stops new requests from being popped and looked up;
 * it says nothing about requests already sitting in the queue (a shutdown
 * with more queued work than workers had time to drain) or about idle
 * sources already posted to the main context. The former are drained and
 * freed directly here — nothing else will ever pop them once no worker
 * remains. The latter are not waited on: each holds its own ref on self (see
 * thumb_loader_post_result), so this object is kept alive by them for as long
 * as any are outstanding, and finalize simply does not run until the last one
 * lets go. Setting stopped first makes those already-queued deliveries
 * harmless no-ops (thumb_loader_deliver) rather than callbacks into a caller
 * that may itself be mid-teardown; it is set here, not in dispose, because
 * ctx must already be unusable to every worker before any such delivery could
 * run.
 *
 * Idempotent: a second call finds n_threads workers already joined and NULL
 * (self->workers was cleared by the first call), so it repeats only the
 * queue-drain, which is harmless on an already-empty queue. */
void thumb_loader_shutdown(ThumbLoader* self) {
    self->stopped = TRUE;

    if (self->n_threads > 0 && self->workers != NULL) {
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
}

/* Falls back to thumb_loader_shutdown for callers that drop the last ref
 * without having called it explicitly (every existing test does this): ctx
 * must not be relied upon by a worker beyond this object's own lifetime
 * regardless of how the caller chose to tear it down. Idempotent, so this is
 * a no-op when main() has already called thumb_loader_shutdown. */
static void thumb_loader_dispose(GObject* obj) {
    ThumbLoader* self = PHIG_THUMB_LOADER(obj);
    thumb_loader_shutdown(self);
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

/* Returns a ref to an already-cached texture for (sha256, size), or NULL.
 * Touches the LRU on a hit. */
static GdkTexture* thumb_loader_cache_probe(ThumbLoader* self, const char* sha256, guint32 size) {
    char* key = thumb_cache_key(sha256, size);
    g_mutex_lock(&self->cache_lock);
    ThumbCacheEntry* entry = g_hash_table_lookup(self->cache, key);
    GdkTexture* ref = NULL;
    if (entry != NULL) {
        g_queue_unlink(&self->lru, entry->lru_link);
        g_queue_push_tail_link(&self->lru, entry->lru_link);
        ref = g_object_ref(entry->texture);
    }
    g_mutex_unlock(&self->cache_lock);
    g_free(key);
    return ref;
}

void thumb_loader_request(ThumbLoader* self, int64_t image_id, const char* sha256,
                           guint32 max_dim, GCancellable* cancellable, ThumbCallback cb,
                           gpointer user_data) {
    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
        return; /* never dispatched, so cb must not run: matches the header's contract */

    /* Probe before dispatching: without this, a rebind onto an already-cached
     * texture still pays a phig_thumb_lookup plus a file decode on a worker
     * (~0.5ms) before the cache is consulted in thumb_loader_deliver. Matters
     * more now that zoom re-requests every visible cell at a new size and the
     * previous size may still be resident. */
    GdkTexture* cached = thumb_loader_cache_probe(self, sha256, max_dim);
    if (cached != NULL) {
        cb(THUMB_OK, cached, user_data);
        g_object_unref(cached);
        return;
    }

    ThumbRequest* req = g_new0(ThumbRequest, 1);
    req->loader = self;
    req->image_id = image_id;
    req->max_dim = max_dim;
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
