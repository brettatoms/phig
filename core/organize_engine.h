#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "progress_queue.h"

// How one file's destination collision is resolved. Mirrors the strings
// resolve_conflict() takes; the engine converts at that call site rather than
// changing a primitive other callers share.
enum class ConflictResolution { Skip, Overwrite, Rename };

struct OrganizeOptions {
    std::string destination;
    std::string format;
    // The rows to act on. Empty selects nothing — a caller names its subject,
    // and forgetting to is a no-op rather than a whole-collection move. cli/
    // resolves its --match/--exclude globs through Database::ids_matching
    // before filling this.
    std::vector<int64_t> ids;
    ConflictResolution on_conflict = ConflictResolution::Rename;
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

// One planned move or copy. `dest` is empty when the item resolved to a skip.
struct OrganizeAction {
    std::string source;
    std::string dest;
    // The destination before conflict resolution, kept so an override can be
    // recomputed from the same starting point rather than from a renamed path.
    std::string intended_dest;
    // The destination was occupied on disk, or already claimed by an earlier
    // item in this same plan.
    bool conflicts = false;
    // Whether the occupying file has a row in the database. Meaningless unless
    // `conflicts`. False means phig cannot restore it from its own records,
    // which is when a client should warn hardest.
    bool occupant_tracked = false;
    ConflictResolution resolution = ConflictResolution::Rename;
    // The caller chose this resolution explicitly rather than inheriting the
    // run-level policy. Carries the authority that lets apply() replace a file
    // that exists on disk.
    bool overridden = false;
};

// A caller-owned plan. Editable: a caller may override the resolution of any
// conflicting item before handing it to apply(). Owning the plan is what makes
// that safe — no engine thread shares this object.
class OrganizePlan {
   public:
    size_t size() const { return actions_.size(); }
    const OrganizeAction& at(size_t i) const { return actions_.at(i); }

    // Override one item's conflict resolution, recomputing its destination so
    // at(i).dest reflects the choice. Returns false, changing nothing, when `i`
    // is out of range, the item does not conflict, or the resulting destination
    // would be another image's not-yet-moved source file — which no caller
    // intent may overwrite.
    bool set_resolution(size_t i, ConflictResolution r);

   private:
    std::vector<OrganizeAction> actions_;
    // Every source path in this plan. A destination equal to one of these is a
    // tracked original that has not moved yet.
    std::set<std::string> batch_sources_;
    // Destinations already taken by earlier items, so two sources cannot land
    // on one path.
    std::set<std::string> claimed_;
    friend class OrganizeEngine;
};

// Published once when planning finishes, then once per file as apply() runs.
enum class OrganizeStage { FileDone, Planned };

struct OrganizeProgress {
    OrganizeStage stage = OrganizeStage::FileDone;
    // Parents — the photos the caller selected. Sidecars and attached JPEGs
    // travel with their parent and are counted in `files_written`, never here.
    size_t done = 0, total = 0;
    size_t files_written = 0;
    size_t skipped = 0, errors = 0;
    std::string path;
    // Non-empty when this file failed: already-formatted warning text the
    // CALLER logs. Matches ScanProgress and ThumbsProgress — the engine does
    // not log from inside the loop a caller is rendering progress from.
    std::string error;
};

struct OrganizeResult {
    size_t applied = 0, skipped = 0, errors = 0;
    size_t files_written = 0;
    bool completed = true;
};

// Plans and executes cp/mv. `is_move` is a parameter of apply() rather than of
// the options because one plan serves both: destinations and conflicts are
// identical either way, and only the executor differs.
class OrganizeEngine {
   public:
    OrganizeEngine(const OrganizeOptions& opts, Database& db);

    // Writes nothing. Polls the token between items.
    OrganizePlan plan(const CancelToken& cancel, ProgressQueue<OrganizeProgress>* progress);

    OrganizeResult apply(const OrganizePlan& plan, bool is_move, const CancelToken& cancel,
                         ProgressQueue<OrganizeProgress>* progress);

   private:
    // By value: constructing from a temporary would otherwise dangle.
    OrganizeOptions opts_;
    Database& db_;
};
