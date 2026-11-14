#include "duplicates_resolve.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include "dates.h"
#include "hasher.h"
#include "organize_exec.h"
#include "paths.h"
#include "remove_exec.h"

namespace fs = std::filesystem;

KeepRule parse_keep_rule(const std::string& spec) {
    KeepRule r;
    if (spec == "unsuffixed") {
        r.kind = KeepRule::Kind::Unsuffixed;
    } else if (spec == "oldest") {
        r.kind = KeepRule::Kind::Oldest;
    } else if (spec == "shortest") {
        r.kind = KeepRule::Kind::Shortest;
    } else if (spec.rfind("under:", 0) == 0 && spec.size() > 6) {
        r.kind = KeepRule::Kind::Under;
        r.dir = spec.substr(6);
    } else {
        throw InvalidOption("duplicates resolve: unknown keep rule '" + spec +
                            "' (use under:<dir>, unsuffixed, oldest or shortest)");
    }
    return r;
}

std::vector<KeepRule> default_keep_rules() {
    return {{KeepRule::Kind::Unsuffixed, ""},
            {KeepRule::Kind::Oldest, ""},
            {KeepRule::Kind::Shortest, ""}};
}

namespace {

bool is_under(const std::string& path, const std::string& dir) {
    fs::path root = fs::path(dir).lexically_normal();
    if (!root.has_filename() && root.has_relative_path()) root = root.parent_path();
    const fs::path rel = fs::path(path).lexically_normal().lexically_relative(root);
    return !rel.empty() && rel != "." && *rel.begin() != "..";
}

// Named like a `_N` copy of another member: "IMG_1_2.jpg" beside "IMG_1.jpg".
bool is_suffixed_copy(const std::string& filename, const std::set<std::string>& names) {
    const fs::path p(filename);
    const std::string stem = p.stem().string();
    const size_t us = stem.rfind('_');
    if (us == std::string::npos || us + 1 == stem.size()) return false;
    if (!std::all_of(stem.begin() + us + 1, stem.end(),
                     [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    return names.count(stem.substr(0, us) + p.extension().string()) > 0;
}

fs::file_time_type mtime_of(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::max() : t;
}

// Narrows `candidates` (indices into `members`) by one rule. Returns them
// unchanged when the rule picks none of them.
std::vector<size_t> narrow(const KeepRule& rule, const std::vector<ImageInfo>& members,
                           const std::vector<size_t>& candidates) {
    std::vector<size_t> out;
    switch (rule.kind) {
        case KeepRule::Kind::Under:
            for (size_t i : candidates)
                if (is_under(members[i].path, rule.dir)) out.push_back(i);
            break;
        case KeepRule::Kind::Unsuffixed: {
            std::set<std::string> names;
            for (const auto& m : members) names.insert(m.filename);
            for (size_t i : candidates)
                if (!is_suffixed_copy(members[i].filename, names)) out.push_back(i);
            break;
        }
        case KeepRule::Kind::Oldest: {
            auto best = fs::file_time_type::max();
            for (size_t i : candidates) {
                const auto t = mtime_of(members[i].path);
                if (plausible_mtime(t)) best = std::min(best, t);
            }
            for (size_t i : candidates)
                if (best != fs::file_time_type::max() && mtime_of(members[i].path) == best)
                    out.push_back(i);
            break;
        }
        case KeepRule::Kind::Shortest: {
            size_t best = SIZE_MAX;
            for (size_t i : candidates) best = std::min(best, members[i].path.size());
            for (size_t i : candidates)
                if (members[i].path.size() == best) out.push_back(i);
            break;
        }
    }
    return out.empty() ? candidates : out;
}

KeepReason reason_for(KeepRule::Kind kind) {
    switch (kind) {
        case KeepRule::Kind::Under:
            return KeepReason::Under;
        case KeepRule::Kind::Unsuffixed:
            return KeepReason::Unsuffixed;
        case KeepRule::Kind::Oldest:
            return KeepReason::Oldest;
        case KeepRule::Kind::Shortest:
            return KeepReason::Shortest;
    }
    return KeepReason::LowestId;
}

// Members are in id order, so the first remaining candidate has the lowest id.
std::pair<size_t, KeepReason> choose_keeper(const std::vector<ImageInfo>& members,
                                            const std::vector<KeepRule>& rules) {
    std::vector<size_t> candidates(members.size());
    for (size_t i = 0; i < members.size(); ++i) candidates[i] = i;
    for (const auto& rule : rules) {
        candidates = narrow(rule, members, candidates);
        if (candidates.size() == 1) return {candidates[0], reason_for(rule.kind)};
    }
    return {candidates[0], KeepReason::LowestId};
}

// Why the group, with its current keeper, cannot be resolved without losing
// something, or None. Reads faces and attachments from the database and disk.
ResolveBlocker check_group(Database& db, const ResolveGroup& g) {
    if (g.members.size() < 2) return ResolveBlocker::TooFewOnDisk;

    std::set<std::string> galleries;
    for (const auto& m : g.members)
        if (!m.gallery.empty()) galleries.insert(m.gallery);
    if (galleries.size() > 1) return ResolveBlocker::DifferentGalleries;

    // R37 moves faces only onto a target with none, so named faces on a removed
    // member survive only when nothing else brings faces to the keeper first.
    const ImageInfo& keeper = g.members[g.keeper];
    size_t face_bearers = db.has_faces(keeper.path) ? 1 : 0;
    bool named = false;
    for (size_t i = 0; i < g.members.size(); ++i) {
        if (i == g.keeper) continue;
        if (db.has_faces(g.members[i].path)) face_bearers++;
        if (!db.get_labelled_faces_for_image(g.members[i].id).empty()) named = true;
    }
    if (named && face_bearers > 1) return ResolveBlocker::NamedFaces;

    // Every attachment lands beside the keeper: it must match what is there
    // already, or what another member is about to carry there.
    std::map<fs::path, fs::path> carried;
    for (size_t i = 0; i < g.members.size(); ++i) {
        if (i == g.keeper) continue;
        for (const auto& m : attachment_moves(db, g.members[i].path, keeper.path)) {
            std::error_code ec;
            if (fs::exists(m.dest, ec) || ec) {
                if (!same_bytes(m.source.string(), m.dest.string()))
                    return ResolveBlocker::AttachmentDiffers;
                continue;
            }
            auto [it, inserted] = carried.emplace(m.dest, m.source);
            if (!inserted && !same_bytes(m.source.string(), it->second.string()))
                return ResolveBlocker::AttachmentDiffers;
        }
    }
    return ResolveBlocker::None;
}

struct GroupOutcome {
    bool resolved = false;
    size_t trashed = 0;
    CarryResult carry;
};

// Resolves one group, re-checking it from disk and the database first. Throws
// on a failure after anything changed, with the trashed files and the keeper's
// mtime already restored.
GroupOutcome resolve_group(Database& db, const ResolveGroup& planned, int64_t op_id) {
    GroupOutcome out;
    const bool logging = op_id > 0;

    // Fresh rows: a gallery or a path may have changed since planning.
    std::vector<int64_t> ids;
    ids.reserve(planned.members.size());
    for (const auto& m : planned.members) ids.push_back(m.id);
    std::map<int64_t, ImageInfo> fresh;
    for (auto& r : db.get_images_by_ids(ids)) fresh.emplace(r.id, std::move(r));

    auto still_identical = [&](const ImageInfo& m) {
        auto it = fresh.find(m.id);
        if (it == fresh.end() || it->second.path != m.path) return false;
        try {
            return compute_sha256(m.path) == planned.sha256;
        } catch (const std::exception&) {
            return false;
        }
    };

    ResolveGroup g;
    g.sha256 = planned.sha256;
    const ImageInfo& planned_keeper = planned.members.at(planned.keeper);
    if (!still_identical(planned_keeper)) return out;
    g.members.push_back(fresh.at(planned_keeper.id));
    for (size_t i = 0; i < planned.members.size(); ++i) {
        if (i == planned.keeper) continue;
        if (still_identical(planned.members[i]))
            g.members.push_back(fresh.at(planned.members[i].id));
    }
    g.keeper = 0;
    if (check_group(db, g) != ResolveBlocker::None) return out;

    const ImageInfo& keeper = g.members[0];
    const std::vector<ImageInfo> removed(g.members.begin() + 1, g.members.end());

    // Read before anything moves: the attachments' names come from the files.
    std::vector<AttachmentMove> moves;
    for (const auto& m : removed) {
        auto ms = attachment_moves(db, m.path, keeper.path);
        moves.insert(moves.end(), ms.begin(), ms.end());
    }

    const fs::file_time_type keeper_mtime = fs::last_write_time(keeper.path);
    fs::file_time_type final_mtime = keeper_mtime;
    for (const auto& m : removed)
        final_mtime = mtime_to_keep(keeper, final_mtime, m, fs::last_write_time(m.path));
    const bool retime = final_mtime != keeper_mtime;

    std::vector<std::pair<std::string, std::string>> trashed;  // original, trash path
    auto undo = [&] {
        for (auto it = trashed.rbegin(); it != trashed.rend(); ++it) {
            try {
                move_file(it->second, it->first);
            } catch (const std::exception& e) {
                throw std::runtime_error("could not restore " + it->first + " from the trash at " +
                                         it->second + ": " + e.what());
            }
        }
        if (retime) {
            std::error_code ec;
            fs::last_write_time(keeper.path, keeper_mtime, ec);
        }
    };

    try {
        if (retime) fs::last_write_time(keeper.path, final_mtime);
        // Unlogged: the trash entries commit with the row changes below, so the
        // log never records a trash that was undone.
        for (const auto& m : removed)
            trashed.emplace_back(m.path, execute_trash_action(db, m.path, 0));

        db.begin_transaction();
        try {
            std::vector<std::string> paths;
            for (const auto& m : removed) {
                db.transfer_user_metadata(m.id, keeper.id);
                if (logging) db.log_add_entry(op_id, {"relink", m.path, keeper.path, ""});
                paths.push_back(m.path);
            }
            db.delete_paths_logged_in_txn(paths, op_id);
            db.set_modified_at_in_txn(keeper.id, format_time(final_mtime));
            if (retime && logging) {
                db.log_add_entry(op_id,
                                 {"set_mtime", keeper.path, keeper.path,
                                  "{\"previous_mtime\":\"" + format_time(keeper_mtime) + "\"}"});
            }
            if (logging)
                for (const auto& [orig, to] : trashed)
                    db.log_add_entry(op_id, {"trash", orig, to, ""});
            db.commit_transaction();
        } catch (...) {
            db.rollback_transaction();
            throw;
        }
    } catch (...) {
        undo();
        throw;
    }

    out.resolved = true;
    out.trashed = removed.size();
    // Attachments follow once the photos are resolved (R44).
    out.carry = settle_attachments(db, moves, /*is_move=*/true, op_id, keeper.id);
    return out;
}

}  // namespace

bool DuplicatesPlan::set_keeper(size_t group, size_t member) {
    if (group >= groups_.size()) return false;
    ResolveGroup& g = groups_[group];
    if (g.blocker == ResolveBlocker::Skipped || member >= g.members.size()) return false;
    g.keeper = member;
    g.reason = KeepReason::Chosen;
    if (db_) g.blocker = check_group(*db_, g);
    return true;
}

bool DuplicatesPlan::set_skip(size_t group) {
    if (group >= groups_.size()) return false;
    groups_[group].blocker = ResolveBlocker::Skipped;
    return true;
}

DuplicatesResolver::DuplicatesResolver(const ResolveOptions& opts, Database& db)
    : opts_(opts), db_(db) {}

DuplicatesPlan DuplicatesResolver::plan() {
    DuplicatesPlan out;
    out.db_ = &db_;
    const std::set<int64_t> selected(opts_.ids.begin(), opts_.ids.end());

    for (const auto& rows : db_.get_exact_duplicates()) {
        std::vector<int64_t> ids;
        bool shown = false;
        for (const auto& r : rows) {
            const int64_t id = db_.get_image_id(r.path);
            if (id < 0) continue;
            ids.push_back(id);
            if (selected.count(id)) shown = true;
        }
        if (!shown) continue;

        ResolveGroup g;
        g.sha256 = rows.front().sha256;
        auto members = db_.get_images_by_ids(ids);
        std::sort(members.begin(), members.end(),
                  [](const ImageInfo& a, const ImageInfo& b) { return a.id < b.id; });
        for (auto& m : members) {
            std::error_code ec;
            if (fs::exists(m.path, ec) && !ec) g.members.push_back(std::move(m));
        }
        if (g.members.size() >= 2) {
            std::tie(g.keeper, g.reason) = choose_keeper(g.members, opts_.keep);
        }
        g.blocker = check_group(db_, g);
        out.groups_.push_back(std::move(g));
    }
    return out;
}

ResolveResult DuplicatesResolver::apply(const DuplicatesPlan& plan, const CancelToken& cancel,
                                        ProgressQueue<ResolveProgress>* progress) {
    ResolveResult result;
    int64_t op_id = 0;
    if (opts_.log_enabled) {
        op_id = db_.log_begin_operation("duplicates-resolve", opts_.argv_str, iso_utc_now());
    }

    for (size_t i = 0; i < plan.size(); ++i) {
        if (cancel.stop()) {
            result.cancelled = true;
            break;
        }
        const ResolveGroup& g = plan.at(i);
        ResolveProgress p;
        p.total = plan.size();
        p.sha256 = g.sha256;

        if (g.blocker != ResolveBlocker::None) {
            result.skipped++;
        } else {
            try {
                GroupOutcome o = resolve_group(db_, g, op_id);
                if (!o.resolved) {
                    result.skipped++;
                } else {
                    result.resolved++;
                    result.trashed += o.trashed;
                    if (!o.carry.failures.empty()) {
                        result.attachment_errors += o.carry.failures.size();
                        std::string joined;
                        for (const auto& f : o.carry.failures)
                            joined += (joined.empty() ? "" : "; ") + f;
                        p.error = "Resolved the copies of " + g.members.at(g.keeper).path +
                                  ", but did not carry: " + joined;
                    }
                }
            } catch (const std::exception& e) {
                result.errors++;
                p.error = "Failed to resolve the copies of " + g.members.at(g.keeper).path + ": " +
                          e.what();
            }
        }
        if (progress) {
            p.done = i + 1;
            p.trashed = result.trashed;
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
