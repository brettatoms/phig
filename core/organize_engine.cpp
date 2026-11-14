#include "organize_engine.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

#include "dates.h"
#include "hasher.h"
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

struct ClaimedBy {
    std::string sha256;
    int64_t id = -1;
};

// Whether the file at `dest` — on disk, or the one an earlier item in this plan
// will write there — holds `img`'s bytes and can absorb it without losing an
// attachment or user metadata (plan 076). Advisory, like the rest of the plan:
// apply() re-checks every condition from disk before acting.
bool identical_occupant(Database& db, const ImageInfo& img, const std::string& dest, bool on_disk,
                        const std::set<std::string>& batch_sources,
                        const std::map<std::string, ClaimedBy>& claims) {
    if (img.sha256.empty()) return false;
    if (!on_disk) {
        auto it = claims.find(dest);
        if (it == claims.end() || it->second.sha256 != img.sha256) return false;
        return metadata_merge_blocker(db, img.id, it->second.id).empty();
    }
    // Another image this run is about to move away: it is not staying, so it is
    // not a copy this one can merge into.
    if (batch_sources.count(dest)) return false;

    std::error_code ec_a, ec_b;
    const auto src_size = fs::file_size(img.path, ec_a);
    const auto dst_size = fs::file_size(dest, ec_b);
    if (ec_a || ec_b || src_size != dst_size) return false;

    const int64_t occupant_id = db.get_image_id(dest);
    std::string occupant_sha;
    if (occupant_id >= 0) {
        occupant_sha = db.get_sha256(dest).value_or("");
    } else {
        try {
            occupant_sha = compute_sha256(dest);
        } catch (const std::exception&) {
            return false;
        }
    }
    if (occupant_sha != img.sha256) return false;
    if (!attachments_mergeable(attachment_moves(db, img.path, dest))) return false;
    return occupant_id < 0 || metadata_merge_blocker(db, img.id, occupant_id).empty();
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

void validate(const OrganizeOptions& opts) {
    // An empty destination would resolve every planned path against the process's
    // working directory; an empty format would send every file to the same path.
    if (opts.destination.empty()) throw InvalidOption("organize: no destination given");
    if (opts.format.empty()) throw InvalidOption("organize: empty format");
    // The format is joined onto the destination, so an absolute one would replace it
    // and a ".." would climb out of it (R87).
    const fs::path fmt(opts.format);
    if (fmt.has_root_path()) {
        throw InvalidOption("organize: format '" + opts.format +
                            "' is an absolute path; it must be relative to the destination");
    }
    for (const auto& part : fmt) {
        if (part == "..") {
            throw InvalidOption("organize: format '" + opts.format +
                                "' leaves the destination through '..'");
        }
    }
    if (std::string bad = first_unknown_token(opts.format); !bad.empty()) {
        throw InvalidOption("organize: unknown token '" + bad + "' in format '" + opts.format +
                            "'");
    }
}

OrganizePlan OrganizeEngine::plan(const CancelToken& cancel,
                                  ProgressQueue<OrganizeProgress>* progress) {
    validate(opts_);
    OrganizePlan out;

    auto images = db_.get_images_by_ids(opts_.ids);

    for (const auto& img : images) out.batch_sources_.insert(img.path);

    // Each destination an earlier item will write, with that item's sha256 and
    // row id, so a later item carrying the same bytes is recognized as identical
    // rather than renamed around a file that does not exist yet.
    std::map<std::string, ClaimedBy> claims;

    // What every planned destination must lie under. A trailing separator leaves an
    // empty last element that lexically_relative would not match.
    fs::path root = fs::path(opts_.destination).lexically_normal();
    if (!root.has_filename() && root.has_relative_path()) root = root.parent_path();

    for (const auto& img : images) {
        if (cancel.stop()) break;

        DateParts date = get_best_date(img);
        fs::path dest = fs::path(opts_.destination) / apply_format(opts_.format, img, date);
        // Backstop for validate() and apply_format's cleaning, which between them
        // leave nothing that can escape: reaching this is a bug, so no plan is
        // produced rather than one item being skipped (R87).
        const fs::path rel = dest.lexically_normal().lexically_relative(root);
        if (rel.empty() || rel == "." || *rel.begin() == "..") {
            throw std::runtime_error("organize: planned destination " + dest.string() +
                                     " is outside " + opts_.destination);
        }

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

        const bool on_disk = fs::exists(dest);
        const bool occupied = on_disk || out.claimed_.count(dest.string()) > 0;

        OrganizeAction act;
        act.source = img.path;
        act.intended_dest = dest.string();
        act.resolution = opts_.on_conflict;

        if (occupied && dest.string() != img.path &&
            identical_occupant(db_, img, dest.string(), on_disk, out.batch_sources_, claims)) {
            act.identical = true;
            act.dest = dest.string();
        } else {
            act.conflicts = occupied;
            if (occupied) act.occupant_tracked = db_.get_image_id(dest.string()) >= 0;
            act.dest = resolve_conflict(dest.string(), policy_string(opts_.on_conflict),
                                        out.claimed_, is_protected);
            if (!act.dest.empty()) claims[act.dest] = {img.sha256, img.id};
        }
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

        // A destination that already holds these bytes. When nothing is there
        // any more — the earlier item it matched was skipped or failed — the
        // item is written like any other, below.
        if (act.identical) {
            std::error_code there_ec;
            if (fs::exists(act.dest, there_ec) || there_ec) {
                try {
                    IdenticalResult ir =
                        execute_identical_action(db_, act.source, act.dest, is_move, op_id);
                    if (!ir.resolved) {
                        result.skipped++;
                    } else {
                        result.identical++;
                        result.files_written += ir.carry.carried;
                        if (!ir.carry.failures.empty()) {
                            result.attachment_errors += ir.carry.failures.size();
                            std::string joined;
                            for (const auto& f : ir.carry.failures)
                                joined += (joined.empty() ? "" : "; ") + f;
                            p.error = act.source + " was already at " + act.dest +
                                      ", but did not carry: " + joined;
                        }
                    }
                } catch (const std::exception& e) {
                    result.errors++;
                    p.error = "Failed to organize " + act.source + ": " + e.what();
                }
                if (progress) {
                    p.done = result.applied + result.errors + result.identical;
                    p.files_written = result.files_written;
                    p.skipped = result.skipped;
                    p.errors = result.errors;
                    p.identical = result.identical;
                    progress->push(p);
                }
                continue;
            }
        }

        // The plan is advisory; this check is authoritative (plan 041 decision
        // 4, inherited by 042). A destination that became occupied since
        // planning is skipped rather than acted on, unless the caller
        // explicitly authorized replacing what is there.
        const bool authorized_overwrite =
            act.overridden && act.resolution == ConflictResolution::Overwrite;
        std::error_code exists_ec;
        const bool occupied =
            !authorized_overwrite && act.source != act.dest && fs::exists(act.dest, exists_ec);
        if (exists_ec) {
            // The destination cannot be inspected (EACCES, ELOOP, ENAMETOOLONG):
            // count the item as an error and carry on, so the run still ends.
            result.errors++;
            p.error = "Failed to organize " + act.source + ": cannot check " + act.dest + ": " +
                      exists_ec.message();
            if (progress) {
                p.done = result.applied + result.errors + result.identical;
                p.identical = result.identical;
                p.skipped = result.skipped;
                p.errors = result.errors;
                p.files_written = result.files_written;
                progress->push(p);
            }
            continue;
        }
        if (occupied) {
            result.skipped++;
            if (progress) {
                p.done = result.applied + result.errors + result.identical;
                p.identical = result.identical;
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
            p.done = result.applied + result.errors + result.identical;
            p.identical = result.identical;
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
