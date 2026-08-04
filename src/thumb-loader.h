#ifndef PHIG_GTK_THUMB_LOADER_H
#define PHIG_GTK_THUMB_LOADER_H

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <phig/phig.h>

G_BEGIN_DECLS

typedef enum {
    THUMB_OK = 0,  /* texture is non-NULL */
    THUMB_MISS,    /* no cached thumbnail; M0 does not generate one */
    THUMB_FAILED   /* the file exists but could not be loaded */
} ThumbResult;

/* Runs on the main thread. `texture` is owned by the loader's cache; ref it to
 * keep it. Not called at all if the request was cancelled before dispatch. */
typedef void (*ThumbCallback)(ThumbResult result, GdkTexture* texture, gpointer user_data);

/* ThumbLoader: an async, cancellable queue that resolves a thumbnail path
 * through phig_thumb_lookup and loads the file into a GdkTexture, backed by a
 * byte-bounded LRU cache of decoded textures.
 *
 * n_threads workers pull requests off a bounded queue; each request looks up
 * the sha via phig_thumb_lookup and, on a hit, loads the file off the main
 * thread. Results are posted back to the main thread with g_idle_add.
 * cache_bytes bounds the decoded-texture cache, charged at width * height * 4
 * per texture; the least-recently-used entry is evicted until the total fits. */
#define PHIG_TYPE_THUMB_LOADER (thumb_loader_get_type())
G_DECLARE_FINAL_TYPE(ThumbLoader, thumb_loader, PHIG, THUMB_LOADER, GObject)

/* Caller keeps ownership of ctx and must not free it while any reference to
 * the loader is outstanding: workers call phig_thumb_lookup on ctx from their
 * own threads for as long as the loader exists, and ctx is only safe for
 * concurrent use for as long as it is not freed out from under a call. */
ThumbLoader* thumb_loader_new(phig_ctx* ctx, guint n_threads, gsize cache_bytes);

/* Request a thumbnail texture for `sha256` (64-char hex).
 *
 * Asynchronous and cancellable. `max_dim` is the largest edge the caller can
 * use; M0's backend ignores it because the cache holds one size, but it is in
 * the signature so a later backend that generates at a requested size — in
 * process or in a phig-decode subprocess — needs no call-site change.
 *
 * On a cache miss this replies THUMB_MISS. M0 never decodes. */
void thumb_loader_request(ThumbLoader* self, const char* sha256, guint32 max_dim,
                           GCancellable* cancellable, ThumbCallback cb, gpointer user_data);

/* Test-only helper: current total bytes charged against cache_bytes. */
gsize thumb_loader_cache_bytes(ThumbLoader* self);

/* Test-only helper: number of requests currently queued, not yet popped by a
 * worker. Lets a test distinguish "never dispatched" (the queue never grew)
 * from "dispatched, then discarded" (it grew and shrank again). */
gint thumb_loader_queue_length(ThumbLoader* self);

/* Test-only helper: number of results a worker has posted for delivery but
 * that have not yet run (or been destroyed undelivered) on the main thread.
 * Lets a test wait for "the worker has finished with this request and handed
 * it to the main thread" without a fixed sleep. */
guint thumb_loader_pending_idle_count(ThumbLoader* self);

G_END_DECLS

#endif /* PHIG_GTK_THUMB_LOADER_H */
