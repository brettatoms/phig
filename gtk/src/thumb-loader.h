#ifndef PHIG_GTK_THUMB_LOADER_H
#define PHIG_GTK_THUMB_LOADER_H

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <phig/phig.h>

G_BEGIN_DECLS

typedef enum {
    THUMB_OK = 0,  /* texture is non-NULL */
    THUMB_MISS,    /* the row has no sha256 yet; nothing to generate from */
    THUMB_FAILED   /* generation failed, or the cached file could not be loaded */
} ThumbResult;

/* Runs on the main thread. `texture` is owned by the loader's cache; ref it to
 * keep it. Not called at all if the request was cancelled before dispatch. */
typedef void (*ThumbCallback)(ThumbResult result, GdkTexture* texture, gpointer user_data);

/* ThumbLoader: an async, cancellable queue that resolves a thumbnail path
 * through phig_thumb_lookup, generating it via phig_thumb_generate on a miss,
 * and loads the file into a GdkTexture, backed by a byte-bounded LRU cache of
 * decoded textures.
 *
 * n_threads workers pull requests off a bounded queue; each request looks up
 * the sha via phig_thumb_lookup, generates on a miss, and loads the file off
 * the main thread. Results are posted back to the main thread with
 * g_idle_add. cache_bytes bounds the decoded-texture cache, charged at
 * width * height * 4 per texture; the least-recently-used entry is evicted
 * until the total fits. */
#define PHIG_TYPE_THUMB_LOADER (thumb_loader_get_type())
G_DECLARE_FINAL_TYPE(ThumbLoader, thumb_loader, PHIG, THUMB_LOADER, GObject)

/* Caller keeps ownership of ctx and must not free it while any reference to
 * the loader is outstanding: workers call phig_thumb_lookup on ctx from their
 * own threads for as long as the loader exists, and ctx is only safe for
 * concurrent use for as long as it is not freed out from under a call. */
ThumbLoader* thumb_loader_new(phig_ctx* ctx, guint n_threads, gsize cache_bytes);

/* Stops and joins every worker thread and drains the request queue, so that
 * once this returns no thread is inside phig_thumb_lookup/phig_thumb_generate
 * on ctx and none ever will be again. The caller must call this before
 * freeing ctx, regardless of how many refs on the loader itself remain
 * outstanding (an idle source posted for delivery holds one of those, and
 * draining the main loop to wait for it is not reliable at shutdown). Safe to
 * call more than once (dispose also calls it, for callers that skip an
 * explicit call); a second call is a no-op. */
void thumb_loader_shutdown(ThumbLoader* self);

/* Request a thumbnail texture for `image_id`/`sha256` (the latter 64-char
 * hex, or empty if the row has not been hashed yet). Both identify the same
 * row; `image_id` is what phig_thumb_generate needs to resolve the path and
 * media type for generation, and the caller already has it on hand
 * (PhigImageRow), so passing it here avoids a second query to reverse it
 * from the sha.
 *
 * Asynchronous and cancellable, except on a texture-cache hit: an already-cached
 * (sha256, max_dim) texture is delivered synchronously before returning, so `cb`
 * may run inside this call. That avoids a worker round-trip plus a redundant
 * decode on every rebind, which matters because zoom re-requests every visible
 * cell at a new size.
 *
 * `max_dim` is the largest edge in PHYSICAL pixels (cell size times the scale
 * factor). The thumbnail is decoded to that size on a worker and cached under
 * (sha256, max_dim), so requests at different sizes do not evict or alias each
 * other. Pass 0 for the cache's native size, unscaled. Callers should quantise
 * the value rather than passing raw pixel sizes, or the cache grows a generation
 * per distinct size.
 *
 * On a cache miss this generates the thumbnail via phig_thumb_generate before
 * replying, so it is far slower than a hit — hundreds of milliseconds versus
 * about one. Replies THUMB_MISS if the row has no sha256; THUMB_FAILED if
 * generation or the subsequent file load fails. */
void thumb_loader_request(ThumbLoader* self, int64_t image_id, const char* sha256,
                           guint32 max_dim, GCancellable* cancellable, ThumbCallback cb,
                           gpointer user_data);

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
