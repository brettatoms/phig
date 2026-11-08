#include <glib.h>
#include <gtk/gtk.h>

#include <memory>
#include <string>

#include "fake-collection.h"
#include "thumb-loader.h"

namespace {

// Every fake thumbnail is 64x64, charged at width * height * 4 bytes.
constexpr gsize kTextureBytes = 64 * 64 * 4;
constexpr gsize kRoomy = 64 * 1024 * 1024;

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

void on_capture(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    auto* capture = static_cast<Capture*>(user_data);
    capture->called = true;
    capture->result = result;
    capture->width = texture != nullptr ? gdk_texture_get_width(texture) : -1;
    if (capture->loop != nullptr) g_main_loop_quit(capture->loop);
}

void on_must_not_run(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    (void)result;
    (void)texture;
    *static_cast<bool*>(user_data) = true;
}

// Requests one thumbnail and runs the main loop until it is delivered, or for
// 5s. A texture-cache hit is delivered inside thumb_loader_request, before the
// loop would start, so the loop only runs when that did not happen.
Capture request_and_wait(ThumbLoader& loader, int64_t id, const std::string& sha, guint32 size) {
    Capture capture;
    capture.loop = g_main_loop_new(nullptr, FALSE);
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, capture.loop);
    thumb_loader_request(loader, id, sha.c_str(), size, nullptr, on_capture, &capture);
    capture.synchronous = capture.called;
    if (!capture.called) g_main_loop_run(capture.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_main_loop_unref(capture.loop);
    capture.loop = nullptr;
    return capture;
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

    thumb_loader_request(*f.loader, 1, sha_of(0).c_str(), 256, cancellable, on_must_not_run,
                         &called);
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

    thumb_loader_request(*f.loader, 1, sha_of(0).c_str(), 256, cancellable, on_must_not_run,
                         &called);
    for (int waited_ms = 0; thumb_loader_pending_idle_count(*f.loader) == 0 && waited_ms < 5000;
         waited_ms++)
        g_usleep(1000);
    g_assert_cmpuint(thumb_loader_pending_idle_count(*f.loader), >, 0);
    g_cancellable_cancel(cancellable);
    pump(300);

    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_cache_bytes(*f.loader), ==, 0);
    g_object_unref(cancellable);
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

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
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
    g_test_add_func("/thumb-loader/the-cache-keeps-the-most-recent-within-budget",
                    test_the_cache_keeps_the_most_recent_within_budget);
    g_test_add_func("/thumb-loader/a-texture-over-budget-is-delivered-but-not-kept",
                    test_a_texture_over_budget_is_delivered_but_not_kept);
    return g_test_run();
}
