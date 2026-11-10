#include "thumb-loader.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gref.h"

namespace {

// One request. Exactly one place owns it at a time: the queue, then the worker
// that took it, then the delivery that carries it back to the main thread. It
// is destroyed only on the main thread, because its callback may hold the last
// reference to a GTK object.
struct ThumbRequest {
    int64_t image_id = 0;
    std::string sha256;
    guint32 max_dim = 0;  // 0 = the cache's native size
    GRef<GCancellable> cancellable;
    ThumbCallback cb;

    // Set by the worker before it hands the request back.
    ThumbResult result = THUMB_MISS;
    GRef<GdkTexture> texture;
};

bool is_cancelled(const ThumbRequest& req) {
    return req.cancellable != nullptr && g_cancellable_is_cancelled(req.cancellable.get());
}

struct CacheEntry {
    GRef<GdkTexture> texture;
    gsize bytes = 0;
    std::list<std::string>::iterator lru;  // this entry's node in ThumbLoader::lru
};

// "<sha256>@<size>": the same image is cached at every size the grid has asked
// for, so a zoom is never served a texture built for the previous size.
std::string cache_key(const std::string& sha256, guint32 size) {
    return sha256 + "@" + std::to_string(size);
}

struct Delivery;

}  // namespace

struct ThumbLoader {
    phig::gtk::Collection* coll = nullptr;  // borrowed; see thumb_loader_new
    gsize cache_bytes_budget = 0;
    std::weak_ptr<ThumbLoader> self;  // what a worker hands to the idle source it posts

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<std::unique_ptr<ThumbRequest>> queue;
    bool stopping = false;  // guarded by queue_mutex
    // Requests a worker finished while the last loader reference was being
    // dropped, so it had no reference to post them with. thumb_loader_shutdown
    // destroys them after the join. Guarded by queue_mutex.
    std::vector<std::unique_ptr<ThumbRequest>> orphans;
    std::vector<std::thread> workers;

    std::mutex cache_mutex;
    std::unordered_map<std::string, CacheEntry> cache;
    std::list<std::string> lru;  // front = least recently used
    gsize cache_bytes = 0;

    // Deliveries posted to the main context and not yet destroyed.
    // thumb_loader_shutdown takes their requests, so a callback is destroyed
    // even if the main context never runs again.
    std::mutex posted_mutex;
    std::unordered_set<Delivery*> posted;

    // Diagnostic only; nothing waits on it.
    std::atomic<guint> pending_idle{0};

    // Set by thumb_loader_shutdown. A delivery already on the main context
    // checks it and delivers nothing, since the caller may be mid-teardown.
    std::atomic<bool> stopped{false};

    ~ThumbLoader() { thumb_loader_shutdown(*this); }
};

namespace {

// Caller holds cache_mutex.
void evict_locked(ThumbLoader& self) {
    while (self.cache_bytes > self.cache_bytes_budget && !self.lru.empty()) {
        auto victim = self.cache.find(self.lru.front());
        self.cache_bytes -= victim->second.bytes;
        self.cache.erase(victim);
        self.lru.pop_front();
    }
}

// Caches texture and returns the reference to deliver. That reference is taken
// before evicting: a texture larger than the whole budget is evicted by its own
// insert, and is still delivered.
GRef<GdkTexture> cache_insert(ThumbLoader& self, const std::string& sha256, guint32 size,
                              GRef<GdkTexture> texture) {
    std::lock_guard lock(self.cache_mutex);
    std::string key = cache_key(sha256, size);
    if (auto it = self.cache.find(key); it != self.cache.end()) {
        // A concurrent request for the same sha and size got here first.
        self.lru.splice(self.lru.end(), self.lru, it->second.lru);
        return gref(it->second.texture.get());
    }
    GRef<GdkTexture> delivered = gref(texture.get());
    gsize bytes = static_cast<gsize>(gdk_texture_get_width(texture.get())) *
                  static_cast<gsize>(gdk_texture_get_height(texture.get())) * 4;
    self.lru.push_back(key);
    self.cache.emplace(std::move(key),
                       CacheEntry{std::move(texture), bytes, std::prev(self.lru.end())});
    self.cache_bytes += bytes;
    evict_locked(self);
    return delivered;
}

// A reference to the cached texture for (sha256, size), or null. A hit becomes
// the most recently used.
GRef<GdkTexture> cache_probe(ThumbLoader& self, const std::string& sha256, guint32 size) {
    std::lock_guard lock(self.cache_mutex);
    auto it = self.cache.find(cache_key(sha256, size));
    if (it == self.cache.end()) return nullptr;
    self.lru.splice(self.lru.end(), self.lru, it->second.lru);
    return gref(it->second.texture.get());
}

// What an idle source carries to the main thread. Holding the loader is what
// keeps it alive until the delivery runs or is destroyed undelivered.
struct Delivery {
    std::shared_ptr<ThumbLoader> loader;
    std::unique_ptr<ThumbRequest> req;  // null once thumb_loader_shutdown took it
};

gboolean deliver(gpointer data) noexcept {
    auto& delivery = *static_cast<Delivery*>(data);
    // The request moves out so it is destroyed when this returns, after the
    // callback: a callback that calls thumb_loader_shutdown does not free
    // itself while it runs.
    std::unique_ptr<ThumbRequest> owned = std::move(delivery.req);
    if (!owned) return G_SOURCE_REMOVE;
    ThumbRequest& req = *owned;
    if (delivery.loader->stopped.load() || is_cancelled(req)) return G_SOURCE_REMOVE;

    if (req.result == THUMB_OK) {
        GRef<GdkTexture> texture =
            cache_insert(*delivery.loader, req.sha256, req.max_dim, std::move(req.texture));
        req.cb(THUMB_OK, texture.get());
    } else {
        req.cb(req.result, nullptr);
    }
    return G_SOURCE_REMOVE;
}

// Runs exactly once per posted request, after deliver or instead of it, on the
// main thread. Deleting the Delivery destroys the request, and may drop the
// last reference to the loader, which then joins its workers here.
void delivery_destroy(gpointer data) noexcept {
    auto* delivery = static_cast<Delivery*>(data);
    ThumbLoader& loader = *delivery->loader;
    {
        std::lock_guard lock(loader.posted_mutex);
        loader.posted.erase(delivery);
    }
    loader.pending_idle--;
    delete delivery;
}

void post_result(std::shared_ptr<ThumbLoader> loader, std::unique_ptr<ThumbRequest> req) {
    ThumbLoader& self = *loader;
    auto* delivery = new Delivery{std::move(loader), std::move(req)};
    {
        std::lock_guard lock(self.posted_mutex);
        self.posted.insert(delivery);
    }
    self.pending_idle++;
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver, delivery, delivery_destroy);
}

// Every request a worker takes leaves through here, resolved or not, so it is
// destroyed on the main thread: by its delivery, or by thumb_loader_shutdown.
void hand_back(ThumbLoader& self, std::unique_ptr<ThumbRequest> req) {
    if (std::shared_ptr<ThumbLoader> loader = self.self.lock()) {
        post_result(std::move(loader), std::move(req));
        return;
    }
    std::lock_guard lock(self.queue_mutex);
    self.orphans.push_back(std::move(req));
}

// Looks the thumbnail up, generates it on a miss, and loads it, setting
// req.result and req.texture. Returns without generating if the request was
// cancelled first.
void resolve(ThumbLoader& self, ThumbRequest& req) {
    // Sizes are independent in phig's cache. The caller quantises max_dim, so
    // this is a small set of sizes rather than one per pixel.
    const int want = static_cast<int>(req.max_dim > 0 ? req.max_dim : PHIG_THUMB_SIZE_DEFAULT);
    phig::gtk::ThumbLocation found = phig::gtk::thumb_lookup(*self.coll, req.sha256, want);

    if (found.state == phig::gtk::ThumbLookup::Absent) {
        // Generation costs hundreds of milliseconds against about one for a
        // load, so a request the user has scrolled past stops here.
        if (is_cancelled(req)) return;
        if (phig::gtk::thumb_generate(*self.coll, req.image_id, want) !=
            phig::gtk::ThumbGenerate::Ok) {
            req.result = THUMB_FAILED;
            return;
        }
        found = phig::gtk::thumb_lookup(*self.coll, req.sha256, want);
    }

    if (found.state == phig::gtk::ThumbLookup::Invalid) {
        req.result = THUMB_FAILED;
    } else if (found.state == phig::gtk::ThumbLookup::Absent) {
        req.result = THUMB_MISS;
    } else {
        GError* error = nullptr;
        // phig wrote the file at the size asked for, so it loads without a
        // rescale, which keeps the texture as small as the budget assumes.
        GdkTexture* texture = gdk_texture_new_from_filename(found.path.c_str(), &error);
        if (texture == nullptr) {
            g_warning("thumb loader: failed to load %s: %s", found.path.c_str(),
                      error != nullptr ? error->message : "unknown error");
            g_clear_error(&error);
            req.result = THUMB_FAILED;
        } else {
            req.result = THUMB_OK;
            req.texture = adopt(texture);
        }
    }
}

void worker_main(ThumbLoader& self) {
    for (;;) {
        std::unique_ptr<ThumbRequest> req;
        {
            std::unique_lock lock(self.queue_mutex);
            self.queue_cv.wait(lock, [&self] { return self.stopping || !self.queue.empty(); });
            if (self.stopping) return;
            req = std::move(self.queue.front());
            self.queue.pop_front();
        }

        // A request cancelled before or during resolving goes back unresolved;
        // its delivery sees the cancel and does not call back. A row phig has
        // not hashed has no cache key: that is data phig has not produced, not
        // a failure, so it reports like a not-yet-generated miss.
        if (!is_cancelled(*req)) {
            if (req->sha256.empty())
                req->result = THUMB_MISS;
            else
                resolve(self, *req);
        }
        hand_back(self, std::move(req));
    }
}

}  // namespace

std::shared_ptr<ThumbLoader> thumb_loader_new(phig::gtk::Collection& coll, guint n_threads,
                                              gsize cache_bytes) {
    auto loader = std::make_shared<ThumbLoader>();
    loader->coll = &coll;
    loader->cache_bytes_budget = cache_bytes;
    loader->self = loader;
    loader->workers.reserve(n_threads);
    for (guint i = 0; i < n_threads; i++)
        loader->workers.emplace_back(worker_main, std::ref(*loader));
    return loader;
}

void thumb_loader_shutdown(ThumbLoader& self) {
    self.stopped = true;
    // Requests are moved out under the locks and destroyed after this function's
    // last lock is released, so no callback's destructor runs under one.
    std::deque<std::unique_ptr<ThumbRequest>> queued;
    {
        std::lock_guard lock(self.queue_mutex);
        self.stopping = true;
        queued.swap(self.queue);
    }
    self.queue_cv.notify_all();
    for (std::thread& worker : self.workers) worker.join();
    self.workers.clear();

    std::vector<std::unique_ptr<ThumbRequest>> dropped;
    {
        std::lock_guard lock(self.queue_mutex);
        dropped.swap(self.orphans);
    }
    {
        std::lock_guard lock(self.posted_mutex);
        for (Delivery* delivery : self.posted)
            if (delivery->req) dropped.push_back(std::move(delivery->req));
    }
}

void thumb_loader_request(ThumbLoader& self, int64_t image_id, std::string sha256, guint32 max_dim,
                          GCancellable* cancellable, ThumbCallback cb) {
    if (self.stopped.load()) return;
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) return;

    if (GRef<GdkTexture> cached = cache_probe(self, sha256, max_dim)) {
        cb(THUMB_OK, cached.get());
        return;
    }

    auto req = std::make_unique<ThumbRequest>();
    req->image_id = image_id;
    req->sha256 = std::move(sha256);
    req->max_dim = max_dim;
    req->cancellable = gref(cancellable);
    req->cb = std::move(cb);
    {
        std::lock_guard lock(self.queue_mutex);
        // req is declared before the lock, so on this return it is destroyed
        // after the lock is released.
        if (self.stopping) return;
        self.queue.push_back(std::move(req));
    }
    self.queue_cv.notify_one();
}

gsize thumb_loader_cache_bytes(ThumbLoader& self) {
    std::lock_guard lock(self.cache_mutex);
    return self.cache_bytes;
}

gint thumb_loader_queue_length(ThumbLoader& self) {
    std::lock_guard lock(self.queue_mutex);
    return static_cast<gint>(self.queue.size());
}

guint thumb_loader_pending_idle_count(ThumbLoader& self) { return self.pending_idle.load(); }
