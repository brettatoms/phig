#include "thumbs_engine.h"

#include <algorithm>
#include <atomic>
#include <thread>

#include "hasher.h"
#include "parallel.h"
#include "selection.h"
#include "thumbs.h"

bool thumb_needs_generating(const ImageInfo& img, bool force) {
    if (img.sha256.empty()) return false;
    if (force) return true;
    return !thumb_exists(img.sha256);
}

ThumbsEngine::ThumbsEngine(const ThumbsOptions& opts, Database& db) : opts_(opts), db_(db) {}

ThumbsResult ThumbsEngine::run(const CancelToken& cancel, ProgressQueue<ThumbsProgress>* progress) {
    ThumbsResult result;

    std::vector<ImageInfo> images = db_.get_all_images();
    if (!opts_.matches.empty() || !opts_.excludes.empty()) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts_.matches,
                                                                 opts_.excludes);
                                    }),
                     images.end());
    }
    result.total_selected = images.size();

    if (progress) {
        ThumbsProgress p;
        p.stage = ThumbsStage::Selected;
        p.total = images.size();
        progress->push(p);
    }

    int num_threads = opts_.parallel > 0
                          ? opts_.parallel
                          : static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));

    std::atomic<size_t> generated{0}, skipped{0}, errors{0};

    auto publish = [&](const ImageInfo& img, bool show_path) {
        if (!progress) return;
        ThumbsProgress p;
        p.stage = ThumbsStage::FileDone;
        p.generated = generated.load();
        p.skipped = skipped.load();
        p.errors = errors.load();
        p.done = p.generated + p.skipped + p.errors;
        p.total = images.size();
        p.path = img.path;
        p.show_path = show_path;
        progress->push(p);
    };

    auto worker = [&](size_t idx, int) {
        const ImageInfo& img = images[idx];

        if (!thumb_needs_generating(img, opts_.force)) {
            skipped.fetch_add(1);
            // An unhashed row produced no progress line in the CLI at all; a
            // cache hit produced the terse tick. Preserve both.
            if (!img.sha256.empty()) publish(img, false);
            return;
        }

        try {
            bool ok = false;
            if (img.media_type == "video") {
                auto kf = extract_video_keyframe(img.path);
                if (!kf.frame.empty()) {
                    ok = generate_thumb(kf.frame, img.sha256);
                }
            } else {
                ok = generate_thumb(img.path, img.sha256);
            }
            if (ok) {
                generated.fetch_add(1);
            } else {
                errors.fetch_add(1);
            }
        } catch (...) {
            errors.fetch_add(1);
        }

        publish(img, true);
    };

    result.completed = parallel_for_each(images.size(), num_threads, cancel, worker);
    result.generated = generated.load();
    result.skipped = skipped.load();
    result.errors = errors.load();
    return result;
}
