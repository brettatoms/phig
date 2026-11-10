#pragma once

#include <gtk/gtk.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "collection.h"

// Size requested when a caller passes max_dim 0. Matches phig's own default, so
// an unsized request hits the cache the CLI populates.
inline constexpr guint32 PHIG_THUMB_SIZE_DEFAULT = 512;

enum ThumbResult {
    THUMB_OK = 0,  // texture is non-NULL
    THUMB_MISS,    // the row has no sha256 yet; nothing to generate from
    THUMB_FAILED   // generation failed, or the cached file could not be loaded
};

// Runs on the main thread. texture is valid for the call; ref it to keep it.
// Not called at all if the request is cancelled before delivery.
//
// The loader destroys the callback exactly once, on the main thread, whether or
// not it ran: after delivery, or when the request is cancelled, dropped by
// thumb_loader_shutdown, or made after it. A capture may therefore hold the
// last reference to a GTK object.
using ThumbCallback = std::move_only_function<void(ThumbResult, GdkTexture*) noexcept>;

// An async, cancellable queue that finds a thumbnail through the collection,
// generating it on a miss, and loads it into a GdkTexture, backed by a
// byte-bounded LRU cache of textures.
//
// n_threads workers pull requests off the queue, look up or generate the file,
// and load it off the main thread. Every request goes back to the main thread
// through an idle source, delivered or not, and each idle holds a reference to
// the loader, so the loader outlives every delivery it has posted. cache_bytes
// bounds the texture cache, charged at width * height * 4 per texture; the
// least recently used entry is evicted until the total fits.
struct ThumbLoader;

// Caller keeps ownership of coll and must not close it before calling
// thumb_loader_shutdown: workers call into it from their own threads.
std::shared_ptr<ThumbLoader> thumb_loader_new(PhigCollection* coll, guint n_threads,
                                              gsize cache_bytes);

// Stops and joins every worker, then destroys every queued request and every
// request posted for delivery but not yet delivered, callbacks included, before
// it returns. Once it returns no thread is inside the collection and none will
// be again. Call it on the main thread, before closing coll, however many
// references to the loader remain: a posted delivery holds one. Safe to call
// more than once; the last reference also calls it.
void thumb_loader_shutdown(ThumbLoader& loader);

// Requests a thumbnail for image_id/sha256 (64 hex digits, or "" if phig has not
// hashed the row). image_id is what generation needs; the caller has it on the
// PhigImageRow, which saves reversing it from the sha.
//
// Call it on the main thread. The callback runs on the main thread too: from an
// idle source, or before this returns on a texture-cache hit. A rebind onto a
// texture already in memory costs no worker round-trip and no second decode,
// which matters because zoom re-requests every visible cell at a new size.
//
// A request made after thumb_loader_shutdown, or with a cancellable already
// cancelled, is dropped without a callback.
//
// max_dim is the largest edge in PHYSICAL pixels (cell size times the scale
// factor); 0 means the cache's native size. Textures are cached under
// (sha256, max_dim), so callers should quantise it, or the cache grows a
// generation per distinct size.
//
// A miss generates the thumbnail before replying: hundreds of milliseconds
// against about one for a hit. Replies THUMB_MISS if the row has no sha256,
// THUMB_FAILED if generation or loading fails.
void thumb_loader_request(ThumbLoader& loader, int64_t image_id, std::string sha256,
                          guint32 max_dim, GCancellable* cancellable, ThumbCallback cb);

// Test-only: bytes currently charged against cache_bytes.
gsize thumb_loader_cache_bytes(ThumbLoader& loader);

// Test-only: requests queued and not yet taken by a worker.
gint thumb_loader_queue_length(ThumbLoader& loader);

// Test-only: results posted for delivery that have not yet run, or been
// destroyed undelivered, on the main thread.
guint thumb_loader_pending_idle_count(ThumbLoader& loader);
