#include "organize_engine.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include "dates.h"
#include "organize_plan.h"
#include "paths.h"
#include "selection.h"

namespace fs = std::filesystem;

namespace {

const char* policy_string(ConflictResolution r) {
    switch (r) {
        case ConflictResolution::Skip:
            return "skip";
        case ConflictResolution::Overwrite:
            return "overwrite";
        case ConflictResolution::Rename:
            return "rename";
    }
    return "rename";
}

}  // namespace

bool OrganizePlan::set_resolution(size_t, ConflictResolution) { return false; }

OrganizeEngine::OrganizeEngine(const OrganizeOptions& opts, Database& db) : opts_(opts), db_(db) {}

OrganizePlan OrganizeEngine::plan(const CancelToken& cancel,
                                  ProgressQueue<OrganizeProgress>* progress) {
    OrganizePlan out;

    auto images = db_.get_all_images();
    if (!opts_.matches.empty() || !opts_.excludes.empty()) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts_.matches,
                                                                 opts_.excludes);
                                    }),
                     images.end());
    }

    for (const auto& img : images) out.batch_sources_.insert(img.path);

    for (const auto& img : images) {
        if (cancel.stop()) break;

        DateParts date = get_best_date(img);
        fs::path dest = fs::path(opts_.destination) / apply_format(opts_.format, img, date);

        // Protect another image's not-yet-moved source, and any real file on
        // disk. Clobbering an untracked file is silent data loss; clobbering a
        // tracked one destroys an image the database still knows about. A file
        // may resolve to its own source, which is a no-op.
        const std::string& self_source = img.path;
        auto is_protected = [&](const std::string& path) -> bool {
            if (path == self_source) return false;
            if (out.batch_sources_.count(path)) return true;
            if (fs::exists(path)) return true;
            return false;
        };

        const bool occupied = fs::exists(dest) || out.claimed_.count(dest.string()) > 0;

        OrganizeAction act;
        act.source = img.path;
        act.intended_dest = dest.string();
        act.resolution = opts_.on_conflict;
        act.conflicts = occupied;
        if (occupied) act.occupant_tracked = db_.get_image_id(dest.string()) >= 0;

        act.dest = resolve_conflict(dest.string(), policy_string(opts_.on_conflict), out.claimed_,
                                    is_protected);
        out.actions_.push_back(std::move(act));
    }

    if (progress) {
        OrganizeProgress p;
        p.stage = OrganizeStage::Planned;
        p.total = out.actions_.size();
        progress->push(p);
    }
    return out;
}

OrganizeResult OrganizeEngine::apply(const OrganizePlan&, bool, const CancelToken&,
                                     ProgressQueue<OrganizeProgress>*) {
    return {};
}
