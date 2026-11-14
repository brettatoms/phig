#include <glib.h>

#include <memory>
#include <string>

#include "background.h"

namespace {

gboolean on_timeout(gpointer user_data) noexcept {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

// Runs the loop until quit, or for 5s.
void run_until_quit(GMainLoop* loop) {
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, loop);
    g_main_loop_run(loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
}

void test_work_runs_off_the_calling_thread_and_done_on_it() {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    GThread* caller = g_thread_self();
    GThread* worked_on = nullptr;
    GThread* delivered_on = nullptr;
    std::string delivered;

    phig::gtk::run_in_background<std::string>(
        [&worked_on]() noexcept {
            worked_on = g_thread_self();
            return std::string("from the worker");
        },
        [&, loop](std::string result) noexcept {
            delivered_on = g_thread_self();
            delivered = std::move(result);
            g_main_loop_quit(loop);
        });
    run_until_quit(loop);
    g_main_loop_unref(loop);

    g_assert_true(worked_on != nullptr && worked_on != caller);
    g_assert_true(delivered_on == caller);
    g_assert_cmpstr(delivered.c_str(), ==, "from the worker");
}

// done may own the last reference to what it delivers into.
void test_done_is_destroyed_after_it_runs() {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    auto owned = std::make_shared<int>(7);
    std::weak_ptr<int> watch = owned;
    int seen = 0;

    phig::gtk::run_in_background<int>([]() noexcept { return 1; },
                                      [owned = std::move(owned), &seen, loop](int n) noexcept {
                                          seen = *owned + n;
                                          g_main_loop_quit(loop);
                                      });
    run_until_quit(loop);
    // The task data is freed once the completion callback has been dispatched.
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    g_main_loop_unref(loop);

    g_assert_cmpint(seen, ==, 8);
    g_assert_true(watch.expired());
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/background/work-runs-off-the-calling-thread-and-done-on-it",
                    test_work_runs_off_the_calling_thread_and_done_on_it);
    g_test_add_func("/background/done-is-destroyed-after-it-runs",
                    test_done_is_destroyed_after_it_runs);
    return g_test_run();
}
