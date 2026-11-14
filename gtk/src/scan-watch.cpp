#include "scan-watch.h"

#include <utility>

namespace phig::gtk {

struct ScanWatch {
    IndexRunPtr run;
    guint source = 0;  // the poll's timeout source; 0 once it has stopped
    ScanProgressFn on_progress;
    ScanDoneFn on_done;
};

void ScanWatchClose::operator()(ScanWatch* watch) const noexcept {
    if (watch->source != 0) g_source_remove(watch->source);
    delete watch;  // the run's deleter cancels it and waits for its thread
}

namespace {

gboolean poll(gpointer data) noexcept {
    auto* watch = static_cast<ScanWatch*>(data);
    if (auto progress = index_poll(*watch->run)) watch->on_progress(*progress);
    auto result = index_result(*watch->run);
    if (!result) return G_SOURCE_CONTINUE;
    // on_done may destroy the watch, so nothing here touches it afterwards.
    watch->source = 0;
    ScanDoneFn done = std::move(watch->on_done);
    done(std::move(*result));
    return G_SOURCE_REMOVE;
}

}  // namespace

std::expected<ScanWatchPtr, std::string> scan_start(const std::string& catalog_dir,
                                                    IndexOptions options,
                                                    ScanProgressFn on_progress,
                                                    ScanDoneFn on_done) noexcept {
    auto run = index_start(catalog_dir, std::move(options));
    if (!run) return std::unexpected(std::move(run.error()));
    ScanWatchPtr watch(
        new ScanWatch{std::move(*run), 0, std::move(on_progress), std::move(on_done)});
    watch->source = g_timeout_add(kScanPollMs, poll, watch.get());
    return watch;
}

void scan_cancel(ScanWatch& watch) noexcept { index_cancel(*watch.run); }

}  // namespace phig::gtk
