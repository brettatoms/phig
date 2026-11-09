#include "organize_engine.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include "dates.h"
#include "organize_exec.h"
#include "organize_plan.h"
#include "paths.h"

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

bool OrganizePlan::set_resolution(size_t i, ConflictResolution r) {
    if (i >= actions_.size()) return false;
    OrganizeAction& act = actions_[i];
    if (!act.conflicts) return false;

    const std::string& target = act.intended_dest;
    const std::string& self_source = act.source;

    // Never overwrite another image's not-yet-moved source: that is a tracked
    // original phig is in the middle of relocating, and no caller intent makes
    // destroying it correct. Rename and skip are unaffected — both go around it.
    if (r == ConflictResolution::Overwrite && target != self_source &&
        batch_sources_.count(target)) {
        return false;
    }

    // Release this item's claim before recomputing, so its own earlier
    // destination does not look like somebody else's.
    if (!act.dest.empty()) claimed_.erase(act.dest);

    // An explicit override carries the authority to replace a file that exists
    // on disk — the caller saw the conflict in the plan and answered it. The
    // batch-source arm above is the part that authority does not extend to, so
    // this predicate deliberately does not consult fs::exists.
    auto is_protected = [&](const std::string& path) -> bool {
        if (path == self_source) return false;
        return batch_sources_.count(path) > 0;
    };

    act.dest = resolve_conflict(target, policy_string(r), claimed_, is_protected);
    act.resolution = r;
    act.overridden = true;
    return true;
}

OrganizeEngine::OrganizeEngine(const OrganizeOptions& opts, Database& db) : opts_(opts), db_(db) {}

OrganizePlan OrganizeEngine::plan(const CancelToken& cancel,
                                  ProgressQueue<OrganizeProgress>* progress) {
    OrganizePlan out;

    auto images = db_.get_images_by_ids(opts_.ids);

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

OrganizeResult OrganizeEngine::apply(const OrganizePlan& plan, bool is_move,
                                     const CancelToken& cancel,
                                     ProgressQueue<OrganizeProgress>* progress) {
    OrganizeResult result;

    // Items that will actually be acted on. Skips are decided at plan time, so
    // a caller's denominator should exclude them rather than counting to a
    // total it can never reach.
    size_t actionable = 0;
    for (size_t i = 0; i < plan.size(); i++) {
        if (!plan.at(i).dest.empty()) actionable++;
    }

    int64_t op_id = 0;
    if (opts_.log_enabled) {
        op_id = db_.log_begin_operation(is_move ? "mv" : "cp", opts_.argv_str, iso_utc_now());
    }

    for (size_t i = 0; i < plan.size(); i++) {
        if (cancel.stop()) {
            result.cancelled = true;
            break;
        }
        const OrganizeAction& act = plan.at(i);
        if (act.dest.empty()) {
            result.skipped++;
            continue;
        }

        OrganizeProgress p;
        p.stage = OrganizeStage::FileDone;
        p.total = actionable;
        p.path = act.source;

        // The plan is advisory; this check is authoritative (plan 041 decision
        // 4, inherited by 042). A destination that became occupied since
        // planning is skipped rather than acted on, unless the caller
        // explicitly authorized replacing what is there.
        const bool authorized_overwrite =
            act.overridden && act.resolution == ConflictResolution::Overwrite;
        if (!authorized_overwrite && act.source != act.dest && fs::exists(act.dest)) {
            result.skipped++;
            if (progress) {
                p.done = result.applied + result.errors;
                p.skipped = result.skipped;
                p.files_written = result.files_written;
                progress->push(p);
            }
            continue;
        }

        bool parent_done = false;
        try {
            // Disk op, DB update and log entry commit together, with on-failure
            // compensation so disk and database never disagree.
            execute_organize_action(db_, act.source, act.dest, is_move, op_id,
                                    authorized_overwrite);
            parent_done = true;
        } catch (const std::exception& e) {
            result.errors++;
            p.error = "Failed to organize " + act.source + ": " + e.what();
        }
        if (parent_done) {
            // The photo landed, so the item counts as applied whatever happens
            // to its attachments; an attachment not carried is counted on its own.
            result.applied++;
            result.files_written++;
            std::vector<std::string> failures;
            try {
                CarryResult carry = carry_sidecars(db_, act.source, act.dest, is_move, op_id);
                result.files_written += carry.carried;
                failures = std::move(carry.failures);
            } catch (const std::exception& e) {
                failures.push_back(e.what());
            }
            if (!failures.empty()) {
                result.attachment_errors += failures.size();
                std::string joined;
                for (const auto& f : failures) joined += (joined.empty() ? "" : "; ") + f;
                p.error = std::string(is_move ? "Moved " : "Copied ") + act.source +
                          ", but did not carry: " + joined;
            }
        }

        if (progress) {
            p.done = result.applied + result.errors;
            p.files_written = result.files_written;
            p.skipped = result.skipped;
            p.errors = result.errors;
            progress->push(p);
        }
    }

    result.completed = !result.cancelled && result.errors == 0 && result.attachment_errors == 0;
    if (opts_.log_enabled) {
        db_.log_finish_operation(op_id, result.completed ? "completed" : "interrupted");
        db_.log_prune(opts_.log_max_entries);
    }
    return result;
}
