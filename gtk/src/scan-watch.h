#pragma once

#include <glib.h>

#include <expected>
#include <functional>
#include <memory>
#include <string>

#include "collection.h"

namespace phig::gtk {

using ScanProgressFn = std::move_only_function<void(const IndexProgress&) noexcept>;
using ScanDoneFn = std::move_only_function<void(std::expected<IndexSummary, std::string>) noexcept>;

// A running index, polled from the main context every kScanPollMs. on_progress
// runs on the main thread for each poll that found progress, and on_done runs
// there once, when the run ends; after on_done the watch is idle and may be
// destroyed, from inside on_done too. Destroying a watch before then cancels
// the run, waits for its thread and calls neither callback again.
inline constexpr guint kScanPollMs = 100;

struct ScanWatch;
struct ScanWatchClose {
    void operator()(ScanWatch* watch) const noexcept;
};
using ScanWatchPtr = std::unique_ptr<ScanWatch, ScanWatchClose>;

std::expected<ScanWatchPtr, std::string> scan_start(const std::string& catalog_dir,
                                                    IndexOptions options,
                                                    ScanProgressFn on_progress,
                                                    ScanDoneFn on_done) noexcept;

// Asks the run to stop; on_done follows with completed false.
void scan_cancel(ScanWatch& watch) noexcept;

}  // namespace phig::gtk
