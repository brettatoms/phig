#include <glib.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "fake-collection.h"
#include "startup.h"

namespace {

gboolean on_timeout(gpointer user_data) {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

struct Outcome {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    std::optional<phig::gtk::OpenResult> result;
    GThread* delivered_on = nullptr;

    Outcome() = default;
    Outcome(const Outcome&) = delete;
    Outcome& operator=(const Outcome&) = delete;
    ~Outcome() { g_main_loop_unref(loop); }
};

phig::gtk::OpenDone record_into(Outcome& outcome) {
    return [&outcome](phig::gtk::OpenResult result) noexcept {
        outcome.result.emplace(std::move(result));
        outcome.delivered_on = g_thread_self();
        g_main_loop_quit(outcome.loop);
    };
}

// Runs the main loop until the open is delivered, or for 5s.
void wait_for(Outcome& outcome) {
    guint timeout_id = g_timeout_add_seconds(5, on_timeout, outcome.loop);
    if (!outcome.result) g_main_loop_run(outcome.loop);
    if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
        g_source_remove(timeout_id);
    g_assert_true(outcome.result.has_value());
}

// What closing the window during startup does: the caller's state is freed
// while the worker is still inside open. The worker must read only its copy.
void test_the_worker_reads_only_its_own_copy() {
    phig::gtk::fake::hold_open(true);
    Outcome outcome;
    auto caller_state = std::make_unique<std::string>("/fake/catalog");

    phig::gtk::open_in_background(*caller_state, record_into(outcome));
    caller_state.reset();
    phig::gtk::fake::hold_open(false);
    wait_for(outcome);

    g_assert_true(outcome.result->has_value());
    std::string opened = phig::gtk::fake::last_opened();
    g_assert_cmpstr(opened.c_str(), ==, "/fake/catalog");
}

void test_a_failed_open_carries_its_reason() {
    Outcome outcome;
    phig::gtk::open_in_background(phig::gtk::fake::kMissingCatalog, record_into(outcome));
    wait_for(outcome);

    g_assert_false(outcome.result->has_value());
    g_assert_cmpstr(outcome.result->error().c_str(), ==, "fake: no catalog at /fake/missing");
}

void test_the_result_is_delivered_on_the_calling_thread() {
    Outcome outcome;
    phig::gtk::open_in_background("/fake/catalog", record_into(outcome));
    wait_for(outcome);

    g_assert_true(outcome.delivered_on == g_thread_self());
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/startup/the-worker-reads-only-its-own-copy",
                    test_the_worker_reads_only_its_own_copy);
    g_test_add_func("/startup/a-failed-open-carries-its-reason",
                    test_a_failed_open_carries_its_reason);
    g_test_add_func("/startup/the-result-is-delivered-on-the-calling-thread",
                    test_the_result_is_delivered_on_the_calling_thread);
    return g_test_run();
}
