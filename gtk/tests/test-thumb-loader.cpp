#include <glib.h>
#include <gtk/gtk.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "fake-collection.h"
#include "thumb-loader.h"

namespace {

// Every fake thumbnail is 64x64, charged at width * height * 4 bytes.
constexpr gsize kTextureBytes = 64 * 64 * 4;
constexpr gsize kRoomy = 64 * 1024 * 1024;

GThread* main_thread = nullptr;  // set by main()

std::string sha_of(guint index) {
    char sha[65];
    fake_collection_sha(index, sha);
    return sha;
}

// A collection of 8 rows and a loader over it. The loader is shut down before
// the collection closes: a delivery still queued on the main context holds a
// loader reference, so dropping this one alone may leave workers running.
struct Fixture {
    PhigCollection* coll;
    std::shared_ptr<ThumbLoader> loader;

    explicit Fixture(gsize budget = kRoomy, guint threads = 4)
        : coll(fake_collection_new(8)), loader(thumb_loader_new(coll, threads, budget)) {}
    ~Fixture() {
        thumb_loader_shutdown(*loader);
        loader.reset();
        phig_collection_close(coll);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

// Runs the default main context for ms milliseconds.
void pump(guint ms) {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_timeout_add(ms, on_timeout, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
}

struct Capture {
    GMainLoop* loop = nullptr;
    bool called = false;
    bool synchronous = false;  // delivered inside thumb_loader_request
    ThumbResult result = THUMB_MISS;
    int width = -1;  // of the delivered texture; -1 if none
};

ThumbCallback capturing(Capture& capture) {
    return [&capture](ThumbResult result, GdkTexture* texture) noexcept {
        capture.called = true;
        capture.result = result;
        capture.width = texture != nullptr ? gdk_texture_get_width(texture) : -1;
        if (capture.loop != nullptr) g_main_loop_quit(capture.loop);
    };
}

// For a request that must never call back.
ThumbCallback must_not_run(bool& called) {
    return [&called](ThumbResult, GdkTexture*) noexcept { called = true; };
}

// How often, and on which threads, the callbacks holding one Probe were
// destroyed.
struct Destroyed {
    std::atomic<int> count{0};
    std::atomic<bool> off_main_thread{false};
};

// A move-only capture that reports its own destruction into a Destroyed.
class Probe {
  public:
    explicit Probe(Destroyed& into) : into_(&into) {}
    Probe(Probe&& other) noexcept : into_(std::exchange(other.into_, nullptr)) {}
    Probe& operator=(Probe&&) = delete;
    ~Probe() {
        if (into_ == nullptr) return;
        into_->count++;
        if (g_thread_self() != main_thread) into_->off_main_thread = true;
    }

  private:
    Destroyed* into_;
};

// Sets called when it runs, and reports its destruction into destroyed.
ThumbCallback probed(Destroyed& destroyed, bool& called) {
    return [probe = Probe(destroyed), &called](ThumbResult, GdkTexture*) noexcept {
        called = true;
    };
}

// Requests one thumbnail and runs the main loop until it is delivered, or for
// 5s. A texture-cache hit is delivered inside thumb_loader_request, before the
// loop would start, so the loop only runs when that did not happen.
Capture request_and_wait(ThumbLoader& loader, int64_t id, const std::string& sha, guint32 size) {
    Capture capture;
    capture.loop = g_main_loop_new(nullptr, FALSE);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, capture.loop);
    thumb_loader_request(loader, id, sha, size, nullptr, capturing(capture));
    capture.synchronous = capture.called;
    if (!capture.called) g_main_loop_run(capture.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_main_loop_unref(capture.loop);
    capture.loop = nullptr;
    return capture;
}

// Polls until a worker has posted a result for delivery, for up to 5s.
void wait_for_posted_result(ThumbLoader& loader) {
    for (int waited_ms = 0; thumb_loader_pending_idle_count(loader) == 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpuint(thumb_loader_pending_idle_count(loader), >, 0);
}

// Polls until the workers have taken every queued request, for up to 5s.
void wait_for_empty_queue(ThumbLoader& loader) {
    for (int waited_ms = 0; thumb_loader_queue_length(loader) > 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpint(thumb_loader_queue_length(loader), ==, 0);
}

void test_a_miss_is_generated_and_delivered() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), PHIG_THUMB_SIZE_DEFAULT);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

// The second loader has an empty texture cache, so its request reaches the
// collection, which finds the file the first one generated.
void test_a_thumbnail_on_disk_is_not_regenerated() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);

    std::shared_ptr<ThumbLoader> second = thumb_loader_new(f.coll, 1, kRoomy);
    Capture c = request_and_wait(*second, 1, sha_of(0), 256);
    thumb_loader_shutdown(*second);
    second.reset();

    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_false(c.synchronous);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 1);
}

void test_a_repeat_request_is_served_from_memory() {
    Fixture f;
    g_assert_false(request_and_wait(*f.loader, 1, sha_of(0), 256).synchronous);
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), 256);
    g_assert_true(c.synchronous);
    g_assert_cmpint(c.result, ==, THUMB_OK);
}

// A row phig has not hashed yet is data phig has not produced, not a failure.
void test_no_sha256_yields_thumb_miss() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, 1, "", 256);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_MISS);
    g_assert_cmpint(c.width, ==, -1);
}

// The row has a sha256 but its thumbnail cannot be generated: a failure the
// user should be able to see.
void test_generation_failure_yields_thumb_failed() {
    Fixture f;
    Capture c = request_and_wait(*f.loader, -1, sha_of(0), 256);
    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_FAILED);
    g_assert_cmpint(c.width, ==, -1);
    g_assert_cmpuint(fake_collection_generate_count(f.coll), ==, 0);
}

// No workers, so nothing pops the queue and its length shows whether the
// request was pushed at all.
void test_cancel_before_dispatch_never_calls_back() {
    Fixture f(kRoomy, 0);
    GCancellable* cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, must_not_run(called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 0);
    pump(300);

    g_assert_false(called);
    g_object_unref(cancellable);
}

// Waits until a worker has posted the result before cancelling, so only the
// delivery-side check can see the cancel.
void test_cancel_after_dispatch_discards_the_result() {
    Fixture f;
    GCancellable* cancellable = g_cancellable_new();
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, must_not_run(called));
    wait_for_posted_result(*f.loader);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
    g_object_unref(cancellable);
}

// A posted delivery holds a loader reference, so dropping the caller's last one
// leaves the loader alive until the delivery runs. It still calls back, and the
// loader is then destroyed on the main thread, before the collection closes.
void test_a_delivery_outlives_the_callers_reference() {
    PhigCollection* coll = fake_collection_new(8);
    std::shared_ptr<ThumbLoader> loader = thumb_loader_new(coll, 4, kRoomy);
    std::weak_ptr<ThumbLoader> watch = loader;
    Capture c;

    thumb_loader_request(*loader, 1, sha_of(0), 256, nullptr, capturing(c));
    wait_for_posted_result(*loader);
    loader.reset();
    g_assert_false(watch.expired());
    pump(300);

    g_assert_true(c.called);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_true(watch.expired());
    phig_collection_close(coll);
}

void test_shutdown_discards_a_posted_result() {
    Fixture f;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, must_not_run(called));
    wait_for_posted_result(*f.loader);
    thumb_loader_shutdown(*f.loader);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_pending_idle_count(*f.loader), ==, 0);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
}

// The texture is in memory, so without the shutdown check this would be a
// synchronous hit.
void test_a_request_after_shutdown_is_dropped() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);
    thumb_loader_shutdown(*f.loader);
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, must_not_run(called));
    pump(100);

    g_assert_false(called);
}

// Six textures against room for three and a half: the three most recent stay.
void test_the_cache_keeps_the_most_recent_within_budget() {
    Fixture f(kTextureBytes * 7 / 2);
    for (guint i = 0; i < 6; i++)
        g_assert_cmpint(request_and_wait(*f.loader, i + 1, sha_of(i), 256).result, ==, THUMB_OK);

    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 3 * kTextureBytes);
    g_assert_true(request_and_wait(*f.loader, 6, sha_of(5), 256).synchronous);
    g_assert_false(request_and_wait(*f.loader, 1, sha_of(0), 256).synchronous);
}

// A texture larger than the whole budget is evicted by its own insert. It is
// still delivered intact, and the cache ends empty.
void test_a_texture_over_budget_is_delivered_but_not_kept() {
    Fixture f(kTextureBytes / 2);
    Capture c = request_and_wait(*f.loader, 1, sha_of(0), 256);
    g_assert_cmpint(c.result, ==, THUMB_OK);
    g_assert_cmpint(c.width, ==, 64);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
}

void test_a_delivered_callback_is_destroyed_once() {
    Fixture f;
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    pump(300);

    g_assert_true(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
}

void test_a_cache_hit_destroys_its_callback_before_returning() {
    Fixture f;
    g_assert_cmpint(request_and_wait(*f.loader, 1, sha_of(0), 256).result, ==, THUMB_OK);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));

    g_assert_true(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

void test_a_request_cancelled_before_dispatch_destroys_its_callback() {
    Fixture f(kRoomy, 0);
    GCancellable* cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, probed(destroyed, called));

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_object_unref(cancellable);
}

void test_a_request_cancelled_after_dispatch_destroys_its_callback() {
    Fixture f;
    GCancellable* cancellable = g_cancellable_new();
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, cancellable, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
    g_object_unref(cancellable);
}

// One worker, held inside the first request's generation, so the second
// request is still queued when it is cancelled. The worker then takes it
// already cancelled, and the callback must still be destroyed on the main
// thread: it may hold the last reference to a GTK object.
void test_a_callback_a_worker_drops_is_destroyed_on_the_main_thread() {
    Fixture f(kRoomy, 1);
    fake_collection_hold_generation(f.coll, TRUE);
    Destroyed first_destroyed;
    bool first_called = false;
    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr,
                         probed(first_destroyed, first_called));
    wait_for_empty_queue(*f.loader);

    GCancellable* cancellable = g_cancellable_new();
    Destroyed destroyed;
    bool called = false;
    thumb_loader_request(*f.loader, 2, sha_of(1), 256, cancellable, probed(destroyed, called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 1);
    g_cancellable_cancel(cancellable);
    fake_collection_hold_generation(f.coll, FALSE);
    pump(300);

    g_assert_true(first_called);
    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_false(destroyed.off_main_thread.load());
    g_object_unref(cancellable);
}

// No workers, so the request is still queued when shutdown drops it.
void test_shutdown_destroys_a_queued_callback() {
    Fixture f(kRoomy, 0);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    g_assert_cmpint(thumb_loader_queue_length(*f.loader), ==, 1);
    thumb_loader_shutdown(*f.loader);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

// No pump before the check: after shutdown the main context may never run
// again, as when g_application_run has returned.
void test_shutdown_destroys_a_posted_callback_without_the_main_loop() {
    Fixture f;
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));
    wait_for_posted_result(*f.loader);
    thumb_loader_shutdown(*f.loader);

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);

    pump(100);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
    g_assert_cmpuint(thumb_loader_pending_idle_count(*f.loader), ==, 0);
}

void test_a_request_after_shutdown_destroys_its_callback() {
    Fixture f;
    thumb_loader_shutdown(*f.loader);
    Destroyed destroyed;
    bool called = false;

    thumb_loader_request(*f.loader, 1, sha_of(0), 256, nullptr, probed(destroyed, called));

    g_assert_false(called);
    g_assert_cmpint(destroyed.count.load(), ==, 1);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    main_thread = g_thread_self();
    g_test_add_func("/thumb-loader/a-miss-is-generated-and-delivered",
                    test_a_miss_is_generated_and_delivered);
    g_test_add_func("/thumb-loader/a-thumbnail-on-disk-is-not-regenerated",
                    test_a_thumbnail_on_disk_is_not_regenerated);
    g_test_add_func("/thumb-loader/a-repeat-request-is-served-from-memory",
                    test_a_repeat_request_is_served_from_memory);
    g_test_add_func("/thumb-loader/no-sha256-yields-thumb-miss", test_no_sha256_yields_thumb_miss);
    g_test_add_func("/thumb-loader/generation-failure-yields-thumb-failed",
                    test_generation_failure_yields_thumb_failed);
    g_test_add_func("/thumb-loader/cancel-before-dispatch-never-calls-back",
                    test_cancel_before_dispatch_never_calls_back);
    g_test_add_func("/thumb-loader/cancel-after-dispatch-discards-the-result",
                    test_cancel_after_dispatch_discards_the_result);
    g_test_add_func("/thumb-loader/a-delivery-outlives-the-callers-reference",
                    test_a_delivery_outlives_the_callers_reference);
    g_test_add_func("/thumb-loader/shutdown-discards-a-posted-result",
                    test_shutdown_discards_a_posted_result);
    g_test_add_func("/thumb-loader/a-request-after-shutdown-is-dropped",
                    test_a_request_after_shutdown_is_dropped);
    g_test_add_func("/thumb-loader/the-cache-keeps-the-most-recent-within-budget",
                    test_the_cache_keeps_the_most_recent_within_budget);
    g_test_add_func("/thumb-loader/a-texture-over-budget-is-delivered-but-not-kept",
                    test_a_texture_over_budget_is_delivered_but_not_kept);
    g_test_add_func("/thumb-loader/a-delivered-callback-is-destroyed-once",
                    test_a_delivered_callback_is_destroyed_once);
    g_test_add_func("/thumb-loader/a-cache-hit-destroys-its-callback-before-returning",
                    test_a_cache_hit_destroys_its_callback_before_returning);
    g_test_add_func("/thumb-loader/a-request-cancelled-before-dispatch-destroys-its-callback",
                    test_a_request_cancelled_before_dispatch_destroys_its_callback);
    g_test_add_func("/thumb-loader/a-request-cancelled-after-dispatch-destroys-its-callback",
                    test_a_request_cancelled_after_dispatch_destroys_its_callback);
    g_test_add_func("/thumb-loader/a-callback-a-worker-drops-is-destroyed-on-the-main-thread",
                    test_a_callback_a_worker_drops_is_destroyed_on_the_main_thread);
    g_test_add_func("/thumb-loader/shutdown-destroys-a-queued-callback",
                    test_shutdown_destroys_a_queued_callback);
    g_test_add_func("/thumb-loader/shutdown-destroys-a-posted-callback-without-the-main-loop",
                    test_shutdown_destroys_a_posted_callback_without_the_main_loop);
    g_test_add_func("/thumb-loader/a-request-after-shutdown-destroys-its-callback",
                    test_a_request_after_shutdown_destroys_its_callback);
    return g_test_run();
}
