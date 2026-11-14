#include <glib.h>

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "fake-collection.h"
#include "scan-watch.h"

namespace {

gboolean on_timeout(gpointer user_data) noexcept {
    g_main_loop_quit(static_cast<GMainLoop*>(user_data));
    return G_SOURCE_REMOVE;
}

struct Outcome {
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    std::vector<phig::gtk::IndexProgress> progress;
    std::optional<std::expected<phig::gtk::IndexSummary, std::string>> done;
    int done_calls = 0;

    Outcome() = default;
    Outcome(const Outcome&) = delete;
    Outcome& operator=(const Outcome&) = delete;
    ~Outcome() { g_main_loop_unref(loop); }

    // Runs the loop until done, or for seconds.
    void wait(guint seconds = 5) {
        guint timeout_id = g_timeout_add_seconds(seconds, on_timeout, loop);
        g_main_loop_run(loop);
        if (g_main_context_find_source_by_id(nullptr, timeout_id) != nullptr)
            g_source_remove(timeout_id);
    }
};

phig::gtk::ScanWatchPtr start(Outcome& outcome, const std::string& directory) {
    auto watch = phig::gtk::scan_start(
        "/fake/catalog", {directory, false, true},
        [&outcome](const phig::gtk::IndexProgress& p) noexcept { outcome.progress.push_back(p); },
        [&outcome](std::expected<phig::gtk::IndexSummary, std::string> result) noexcept {
            outcome.done.emplace(std::move(result));
            outcome.done_calls++;
            g_main_loop_quit(outcome.loop);
        });
    if (!watch) g_error("scan_start failed: %s", watch.error().c_str());
    return std::move(*watch);
}

void test_progress_arrives_then_done_once() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch = start(outcome, "/fake/photos");
    outcome.wait();

    g_assert_cmpuint(outcome.progress.size(), ==, phig::gtk::fake::kIndexFiles);
    g_assert_cmpuint(outcome.progress.back().done, ==, phig::gtk::fake::kIndexFiles);
    g_assert_cmpint(outcome.done_calls, ==, 1);
    g_assert_true(outcome.done->has_value());
    g_assert_true((*outcome.done)->completed);
}

void test_the_options_reach_the_index() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch = start(outcome, "/fake/photos");
    outcome.wait();
    const std::string catalog = phig::gtk::fake::last_index_catalog();
    g_assert_cmpstr(catalog.c_str(), ==, "/fake/catalog");
    const phig::gtk::IndexOptions options = phig::gtk::fake::last_index_options();
    g_assert_cmpstr(options.directory.c_str(), ==, "/fake/photos");
    g_assert_false(options.recursive);
    g_assert_true(options.include_hidden);
}

void test_a_cancelled_scan_reports_it_did_not_complete() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch = start(outcome, "/fake/photos");
    phig::gtk::scan_cancel(*watch);
    outcome.wait();
    g_assert_cmpint(outcome.done_calls, ==, 1);
    g_assert_true(outcome.done->has_value());
    g_assert_false((*outcome.done)->completed);
}

void test_a_failed_scan_carries_its_reason() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch = start(outcome, phig::gtk::fake::kIndexFailDirectory);
    outcome.wait();
    g_assert_false(outcome.done->has_value());
    g_assert_cmpstr(outcome.done->error().c_str(), ==, phig::gtk::fake::kIndexError);
}

// What closing the window during a scan does.
void test_destroying_a_running_watch_stops_its_callbacks() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch = start(outcome, "/fake/photos");
    watch.reset();
    outcome.wait(1);  // nothing quits the loop, so this runs the full second
    g_assert_true(outcome.progress.empty());
    g_assert_cmpint(outcome.done_calls, ==, 0);
}

void test_done_may_destroy_the_watch() {
    Outcome outcome;
    phig::gtk::ScanWatchPtr watch;
    auto started = phig::gtk::scan_start(
        "/fake/catalog", {"/fake/photos", true, false},
        [](const phig::gtk::IndexProgress&) noexcept {},
        [&outcome, &watch](std::expected<phig::gtk::IndexSummary, std::string>) noexcept {
            watch.reset();
            outcome.done_calls++;
            g_main_loop_quit(outcome.loop);
        });
    g_assert_true(started.has_value());
    watch = std::move(*started);
    outcome.wait();
    g_assert_cmpint(outcome.done_calls, ==, 1);
    g_assert_null(watch.get());
}

void test_a_scan_with_no_directory_does_not_start() {
    auto watch = phig::gtk::scan_start(
        "/fake/catalog", {"", true, false}, [](const phig::gtk::IndexProgress&) noexcept {},
        [](std::expected<phig::gtk::IndexSummary, std::string>) noexcept {});
    g_assert_false(watch.has_value());
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/scan-watch/progress-arrives-then-done-once",
                    test_progress_arrives_then_done_once);
    g_test_add_func("/scan-watch/the-options-reach-the-index", test_the_options_reach_the_index);
    g_test_add_func("/scan-watch/a-cancelled-scan-reports-it-did-not-complete",
                    test_a_cancelled_scan_reports_it_did_not_complete);
    g_test_add_func("/scan-watch/a-failed-scan-carries-its-reason",
                    test_a_failed_scan_carries_its_reason);
    g_test_add_func("/scan-watch/destroying-a-running-watch-stops-its-callbacks",
                    test_destroying_a_running_watch_stops_its_callbacks);
    g_test_add_func("/scan-watch/done-may-destroy-the-watch", test_done_may_destroy_the_watch);
    g_test_add_func("/scan-watch/a-scan-with-no-directory-does-not-start",
                    test_a_scan_with_no_directory_does_not_start);
    return g_test_run();
}
