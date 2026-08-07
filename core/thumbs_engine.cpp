#include "thumbs_engine.h"

#include <algorithm>
#include <atomic>
#include <thread>

#include "hasher.h"
#include "log.h"
#include "parallel.h"
#include "selection.h"
#include "thumbs.h"

bool thumb_needs_generating(const ImageInfo& img, bool force, int size) {
    if (img.sha256.empty()) return false;
    if (force) return true;
    return !thumb_exists(img.sha256, size);
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

    auto publish = [&](const ImageInfo& img, bool show_path, const std::string& error = "",
                       const std::string& warning = "") {
        if (!progress) return;
        ThumbsProgress p;
        p.stage = ThumbsStage::FileDone;
        p.generated = generated.load();
        p.skipped = skipped.load();
        p.errors = errors.load();
        p.done = p.generated + p.skipped + p.errors;
        p.total = images.size();
        p.path = img.path;
        p.error = error;
        p.warning = warning;
        p.show_path = show_path;
        progress->push(p);
    };

    auto worker = [&](size_t idx, int) {
        const ImageInfo& img = images[idx];

        if (!thumb_needs_generating(img, opts_.force, opts_.size)) {
            skipped.fetch_add(1);
            // An unhashed row produced no progress line in the CLI at all; a
            // cache hit produced the terse tick. Preserve both.
            if (!img.sha256.empty()) publish(img, false);
            return;
        }

        // Decoding here rather than through generate_thumb(path, ...) is what
        // makes a failure explicable: that overload calls decode_image, which
        // logs OpenCV's message and discards it, then collapses every failure
        // into a bare false. Splitting decode from encode lets each failure keep
        // its own reason.
        std::string error;
        std::string warning;
        try {
            bool ok = false;
            if (img.media_type == "video") {
                auto kf = extract_video_keyframe(img.path);
                if (kf.frame.empty()) {
                    error = "thumbs: " + img.path + " -- could not extract a video keyframe";
                } else {
                    ok = generate_thumb(kf.frame, img.sha256, opts_.size);
                    if (!ok) error = "thumbs: " + img.path + " -- could not encode the thumbnail";
                }
            } else {
                DecodeResult decoded = decode_image_with_warning(img.path);
                if (decoded.img.empty()) {
                    error = "thumbs: " + img.path;
                    if (!decoded.warning.empty()) error += " -- " + decoded.warning;
                } else {
                    // Decoded, but the decoder complained. Reported, not counted:
                    // the file produced a usable thumbnail.
                    if (!decoded.warning.empty()) {
                        warning = "thumbs: " + img.path + " -- " + decoded.warning;
                    }
                    ok = generate_thumb(decoded.img, img.sha256, opts_.size);
                    if (!ok) error = "thumbs: " + img.path + " -- could not encode the thumbnail";
                }
            }
            if (ok) {
                generated.fetch_add(1);
            } else {
                errors.fetch_add(1);
            }
        } catch (const std::exception& e) {
            errors.fetch_add(1);
            error = "thumbs: " + img.path + " -- " + e.what();
        } catch (...) {
            errors.fetch_add(1);
            error = "thumbs: " + img.path + " -- unknown error";
        }

        // Nowhere to route it when the caller declined progress, so log it here
        // rather than dropping it.
        if (progress == nullptr) {
            if (!error.empty()) log_warn(error);
            if (!warning.empty()) log_warn(warning);
        }
        publish(img, true, error, warning);
    };

    // parallel_for_each returns true unconditionally for an empty range, which
    // would misreport an already-cancelled run over an empty selection as
    // completed.
    result.completed = images.empty()
                           ? !cancel.stop()
                           : parallel_for_each(images.size(), num_threads, cancel, worker);
    result.generated = generated.load();
    result.skipped = skipped.load();
    result.errors = errors.load();
    return result;
}
