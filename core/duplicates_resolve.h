#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cancel.h"
#include "database.h"
#include "invalid_option.h"
#include "progress_queue.h"
#include "types.h"

// Keeps one file from each group of byte-identical files and trashes the rest,
// after carrying their user metadata to the file kept (plan 077).

// One step of the rule that picks which member of a group to keep. Each step
// narrows the candidates; the first step that leaves one decides.
struct KeepRule {
    enum class Kind {
        Under,       // members whose path lies under `dir`
        Unsuffixed,  // members not named like a `_N` copy of another member
        Oldest,      // the oldest plausible mtime (on or after 1990, not in the future)
        Shortest,    // the shortest path
    };
    Kind kind = Kind::Unsuffixed;
    std::string dir;  // Under only
};

// "under:<dir>", "unsuffixed", "oldest" or "shortest". Throws InvalidOption
// for anything else, and for "under:" with no directory.
KeepRule parse_keep_rule(const std::string& spec);

// unsuffixed, oldest, shortest.
std::vector<KeepRule> default_keep_rules();

struct ResolveOptions {
    // Rows whose groups to act on. A group is included when any of its members
    // is selected, and its membership always comes from the whole collection,
    // so a copy outside the selection is still a member (plan 002's rule).
    // Empty selects nothing.
    std::vector<int64_t> ids;
    std::vector<KeepRule> keep = default_keep_rules();
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

// Which step chose a group's keeper.
enum class KeepReason { Under, Unsuffixed, Oldest, Shortest, LowestId, Chosen };

// Why a group is left as it is. The first four are decided by plan() and
// re-checked by apply(); Skipped is a caller's set_skip.
enum class ResolveBlocker {
    None,
    DifferentGalleries,  // two members have different non-empty galleries
    NamedFaces,          // a member to remove has named faces the keeper could not take
    AttachmentDiffers,   // a member's sidecar, RAW JPEG or Live Photo video differs from the
                         // one beside the keeper
    TooFewOnDisk,        // fewer than two members exist on disk
    Skipped,
};

struct ResolveGroup {
    std::string sha256;
    std::vector<ImageInfo> members;  // in id order; only those on disk
    size_t keeper = 0;               // index into members
    KeepReason reason = KeepReason::LowestId;
    ResolveBlocker blocker = ResolveBlocker::None;
};

// A caller-owned plan. Editable before apply(): a caller may choose another
// keeper for a group, or leave a group alone.
class DuplicatesPlan {
   public:
    size_t size() const { return groups_.size(); }
    const ResolveGroup& at(size_t i) const { return groups_.at(i); }

    // Keep `member` of group `group` instead. Re-checks the group against the new
    // keeper, so its blocker may change. Returns false, changing nothing, when
    // either index is out of range or the group was skipped.
    bool set_keeper(size_t group, size_t member);

    // Leave group `group` alone. Returns false when it is out of range.
    bool set_skip(size_t group);

   private:
    std::vector<ResolveGroup> groups_;
    Database* db_ = nullptr;  // for set_keeper's re-check
    friend class DuplicatesResolver;
};

struct ResolveProgress {
    size_t done = 0, total = 0;  // groups
    size_t trashed = 0;          // files moved to the trash so far
    std::string sha256;          // the group just finished
    // Non-empty when this group failed: already-formatted warning text the
    // caller logs, as with OrganizeProgress.
    std::string error;
};

struct ResolveResult {
    size_t resolved = 0;  // groups now down to one file
    size_t skipped = 0;   // groups left alone: blocked, skipped, or changed since planning
    size_t errors = 0;    // groups that failed and were rolled back
    size_t trashed = 0;   // member files moved to the trash
    size_t attachment_errors = 0;
    bool cancelled = false;
    bool completed = true;  // not cancelled, and nothing failed
};

class DuplicatesResolver {
   public:
    DuplicatesResolver(const ResolveOptions& opts, Database& db);

    // Writes nothing. Reads every member's mtime and attachments from disk.
    DuplicatesPlan plan();

    // Resolves each group that is not blocked. Polls the token between groups,
    // never inside one, and records the run as one activity-log operation.
    ResolveResult apply(const DuplicatesPlan& plan, const CancelToken& cancel,
                        ProgressQueue<ResolveProgress>* progress);

   private:
    ResolveOptions opts_;  // by value: constructing from a temporary would otherwise dangle
    Database& db_;
};
