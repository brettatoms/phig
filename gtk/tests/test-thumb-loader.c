#include <glib.h>
#include <phig/phig.h>
#include "thumb-loader.h"

/* A real cached thumbnail for the hit test: 242x512 baseline JPEG, 14049
 * bytes, one matching row in the test database (see PHIG_TEST_SHA below for
 * how to point this at a different machine's cache). */
#define DEFAULT_HIT_SHA "603048e2b3b3a1c74dedce02521164e11bc985629fa88e840a445045d4e64990"

/* All-zeroes: valid hex, 64 chars, guaranteed not to resolve to a cached file.
 * Paired with a nonexistent id so a lookup miss on it also fails generation —
 * exercising the generate-then-fails path, not a plain cache miss. */
#define GENERATE_FAILS_SHA "0000000000000000000000000000000000000000000000000000000000000000"
#define MISS_ID ((int64_t)-1)

/* Five more real cached thumbnails, used only by the byte-budget test to
 * force eviction. Found by scanning the test database once; not asserted
 * individually, just needed as distinct hits. */
static const char* const kExtraShas[] = {
    "a20b880bdd80bce89c5b39d0f92eabf7c97b26a1a39f6e382cf4170a4bdeef57",
    "9eea968873bc1f2a99677ddd3e30ecfc939d9cbff15135fc81923f9a113f6b31",
    "572f9e8f8ebe21923b6d63f724f589a524858c8c0be691610ea895c0dde8faae",
    "13f2956d4328d27783231bbeafbc2b5ae7934dfdac84cba5045a2ee398f079b9",
    "417c39bbaf47541db111748dd6a9a3a80b134bec25302aabc4aaa411a97620c2",
    "f42c0473aad4a00aa890360a7338509041a70689dd6ee55659137274b745e1d3",
};

typedef struct {
    phig_ctx* ctx;
    ThumbLoader* loader;
    const char* hit_sha;
} Fixture;

static void fixture_set_up(Fixture* f, gconstpointer unused) {
    (void)unused;
    const char* db = g_getenv("PHIG_TEST_DB");
    g_assert_nonnull(db);
    phig_config cfg = {0};
    cfg.db_path = db;
    cfg.mode = PHIG_READ_ONLY;
    g_assert_cmpint(phig_init(&cfg, &f->ctx), ==, PHIG_OK);
    f->loader = thumb_loader_new(f->ctx, 4, 64 * 1024 * 1024);
    f->hit_sha = g_getenv("PHIG_TEST_SHA");
    if (f->hit_sha == NULL)
        f->hit_sha = DEFAULT_HIT_SHA;
}

static void fixture_tear_down(Fixture* f, gconstpointer unused) {
    (void)unused;
    g_clear_object(&f->loader);
    phig_shutdown(f->ctx);
}

static gboolean on_timeout(gpointer user_data) {
    GMainLoop* loop = user_data;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

/* Drives loop with a 5s guard so a bug that drops a callback fails the test
 * instead of hanging CI. Some tests here legitimately expect no callback (a
 * cancelled request), in which case the timeout itself ends the loop and
 * already removed itself by returning G_SOURCE_REMOVE — removing it again
 * would hit a GLib-FATAL-CRITICAL for an unknown source id. Looking the
 * source up first makes disarm_timeout safe regardless of which one fired. */
static guint arm_timeout(GMainLoop* loop) {
    return g_timeout_add_seconds(5, on_timeout, loop);
}

static void disarm_timeout(guint timeout_id) {
    if (g_main_context_find_source_by_id(NULL, timeout_id) != NULL)
        g_source_remove(timeout_id);
}

/* ---- hit yields a texture ---- */

typedef struct {
    GMainLoop* loop;
    gboolean called;
    ThumbResult result;
    GdkTexture* texture; /* reffed if non-NULL, caller must unref */
} Capture;

static void on_thumb_capture(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    Capture* capture = user_data;
    capture->called = TRUE;
    capture->result = result;
    capture->texture = texture != NULL ? g_object_ref(texture) : NULL;
    g_main_loop_quit(capture->loop);
}

static void test_hit_yields_texture(Fixture* f, gconstpointer unused) {
    (void)unused;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    Capture capture = {.loop = loop};
    guint timeout_id = arm_timeout(loop);

    /* hit_sha names an already-cached thumbnail, so the lookup reports a hit
     * and image_id is never read; MISS_ID stands in for "not needed here".
     * The size must be the one actually cached: phig partitions its cache by
     * size, so a request for any other size is a miss that would try to
     * generate. */
    thumb_loader_request(f->loader, MISS_ID, f->hit_sha, PHIG_THUMB_SIZE_DEFAULT, NULL,
                         on_thumb_capture, &capture);
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.called);
    g_assert_cmpint(capture.result, ==, THUMB_OK);
    g_assert_nonnull(capture.texture);
    g_clear_object(&capture.texture);
}

/* ---- no sha256 yields THUMB_MISS ---- */

/* An empty sha256 is what a row gets before phig has hashed it. That is not
 * a failure — there is nothing to look up or generate from yet — so it must
 * report the same as an ordinary not-yet-generated cache miss. */
static void test_no_sha256_yields_thumb_miss(Fixture* f, gconstpointer unused) {
    (void)unused;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    Capture capture = {.loop = loop};
    guint timeout_id = arm_timeout(loop);

    thumb_loader_request(f->loader, MISS_ID, "", 256, NULL, on_thumb_capture, &capture);
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.called);
    g_assert_cmpint(capture.result, ==, THUMB_MISS);
    g_assert_null(capture.texture);
}

/* ---- a lookup miss whose generation also fails yields THUMB_FAILED ---- */

/* Unlike a plain cache miss (nothing generated yet), a failed generation is a
 * real failure the user should be able to notice — phig had a sha256 to work
 * from and still could not produce a thumbnail. */
static void test_generation_failure_yields_thumb_failed(Fixture* f, gconstpointer unused) {
    (void)unused;
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    Capture capture = {.loop = loop};
    guint timeout_id = arm_timeout(loop);

    thumb_loader_request(f->loader, MISS_ID, GENERATE_FAILS_SHA, 256, NULL, on_thumb_capture,
                          &capture);
    g_main_loop_run(loop);

    disarm_timeout(timeout_id);
    g_main_loop_unref(loop);

    g_assert_true(capture.called);
    g_assert_cmpint(capture.result, ==, THUMB_FAILED);
    g_assert_null(capture.texture);
}

/* ---- cancel before dispatch never calls back ---- */

static void on_thumb_must_not_run(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    (void)result;
    (void)texture;
    gboolean* called = user_data;
    *called = TRUE;
}

static void test_cancel_before_dispatch_never_calls_back(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* Uses its own zero-worker loader rather than the fixture's, so nothing
     * can ever pop the queue: that makes thumb_loader_queue_length below a
     * deterministic check of whether thumb_loader_request pushed the request
     * at all, instead of a check that a real worker pool could race and win
     * regardless of whether the pre-dispatch guard ran (a worker's own
     * cancellation check, taken after popping, would otherwise make this
     * test pass for the wrong reason). */
    ThumbLoader* loader = thumb_loader_new(f->ctx, 0, 64 * 1024 * 1024);
    GCancellable* cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);

    gboolean called = FALSE;
    thumb_loader_request(loader, MISS_ID, f->hit_sha, 256, cancellable, on_thumb_must_not_run,
                          &called);

    g_assert_cmpint(thumb_loader_queue_length(loader), <=, 0);

    /* Nothing to wait on: a correct implementation never dispatches this
     * request at all, so there is no completion event to catch. Pump the
     * main loop briefly to give a buggy implementation a chance to fire; the
     * timeout is expected to be what ends the loop here, and removes itself
     * by returning G_SOURCE_REMOVE, so there is nothing left to clean up. */
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(300, on_timeout, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    g_assert_false(called);
    g_object_unref(cancellable);
    g_object_unref(loader);
}

/* ---- cancel after dispatch discards the result ---- */

static void on_thumb_race(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    (void)result;
    gboolean* called = user_data;
    *called = TRUE;
    if (texture != NULL)
        g_object_unref(texture);
}

static void test_cancel_after_dispatch_discards_result(Fixture* f, gconstpointer unused) {
    (void)unused;
    /* Forces the ordering instead of racing it, so this reliably exercises
     * the deliver-side cancellation check in thumb_loader_deliver (just
     * before thumb_loader_cache_insert) rather than either of the two
     * worker-side checks that run earlier (right after popping, and again
     * right after phig_thumb_lookup/gdk_texture_new_from_filename, both
     * before a result is ever posted for delivery). Cancelling immediately
     * after thumb_loader_request — the first approach tried here — was
     * observed to resolve at the pre-lookup check in every run, never
     * reaching delivery, which left the delivery-side check untested.
     *
     * thumb_loader_pending_idle_count only increments once a worker has
     * gotten past both of those checks and actually posted the result (see
     * thumb_loader_post_result), so spin-waiting for it to become nonzero
     * before cancelling guarantees the cancel can only be observed by the
     * delivery-side check — the two earlier ones have already run and let
     * this request through by the time the wait ends. This polls a real
     * state transition rather than sleeping a guessed duration; the 5s cap on
     * the loop below still bounds it in case something regresses and the
     * count never rises. */
    GCancellable* cancellable = g_cancellable_new();
    gboolean called = FALSE;

    thumb_loader_request(f->loader, MISS_ID, f->hit_sha, 256, cancellable, on_thumb_race, &called);
    for (int waited_ms = 0; thumb_loader_pending_idle_count(f->loader) == 0 && waited_ms < 5000;
         waited_ms += 1)
        g_usleep(1000);
    g_cancellable_cancel(cancellable);

    /* Nothing to wait on beyond the guard: a correct implementation never
     * calls on_thumb_race, so there is no completion event to catch — same
     * shape as the before-dispatch test above, just later in the pipeline. */
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add(300, on_timeout, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    /* The deliver-side check must have discarded the cancelled request's
     * result: the callback never runs, and nothing it loaded was cached. */
    g_assert_false(called);
    g_assert_cmpuint(thumb_loader_cache_bytes(f->loader), ==, 0);

    g_object_unref(cancellable);
}

/* ---- cache respects its byte budget ---- */

typedef struct {
    GMainLoop* loop;
    guint remaining;
} BudgetCapture;

static void on_thumb_budget(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    (void)result;
    (void)texture;
    BudgetCapture* capture = user_data;
    capture->remaining--;
    if (capture->remaining == 0)
        g_main_loop_quit(capture->loop);
}

static void test_cache_respects_byte_budget(void) {
    const char* db = g_getenv("PHIG_TEST_DB");
    g_assert_nonnull(db);
    phig_config cfg = {0};
    cfg.db_path = db;
    cfg.mode = PHIG_READ_ONLY;
    phig_ctx* ctx;
    g_assert_cmpint(phig_init(&cfg, &ctx), ==, PHIG_OK);

    /* Room for about 3 of the 6 thumbnails (242*512*4 = ~495KB each), not
     * just 1: a budget that only ever holds a single entry never exercises
     * LRU *ordering* (which entry is evicted first), only bare capacity. */
    gsize budget = 1600 * 1024;
    ThumbLoader* loader = thumb_loader_new(ctx, 4, budget);

    guint n = G_N_ELEMENTS(kExtraShas);
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    BudgetCapture capture = {.loop = loop, .remaining = n};
    guint timeout_id = arm_timeout(loop);

    for (guint i = 0; i < n; i++)
        thumb_loader_request(loader, MISS_ID, kExtraShas[i], 256, NULL, on_thumb_budget, &capture);

    g_main_loop_run(loop);
    disarm_timeout(timeout_id);
    g_main_loop_unref(loop);

    g_assert_cmpuint(capture.remaining, ==, 0); /* all 6 callbacks ran; otherwise the assert below is meaningless */
    g_assert_cmpuint(thumb_loader_cache_bytes(loader), <=, budget);

    g_object_unref(loader);
    phig_shutdown(ctx);
}

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, NULL);
    if (!g_getenv("PHIG_TEST_DB")) {
        g_printerr("PHIG_TEST_DB unset; skipping\n");
        return 77; /* automake/ctest skip convention */
    }
#define T(name, fn) \
    g_test_add("/thumb-loader/" name, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)
    T("hit-yields-texture", test_hit_yields_texture);
    T("no-sha256-yields-thumb-miss", test_no_sha256_yields_thumb_miss);
    T("generation-failure-yields-thumb-failed", test_generation_failure_yields_thumb_failed);
    T("cancel-before-dispatch-never-calls-back", test_cancel_before_dispatch_never_calls_back);
    T("cancel-after-dispatch-discards-result", test_cancel_after_dispatch_discards_result);
#undef T
    g_test_add_func("/thumb-loader/cache-respects-byte-budget", test_cache_respects_byte_budget);
    return g_test_run();
}
