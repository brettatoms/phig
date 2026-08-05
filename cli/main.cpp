#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cancel.h"
#include "config.h"
#include "database.h"
#include "dates.h"
#include "duplicates.h"
#include "exif.h"
#include "faces.h"
#include "faces_engine.h"
#include "hasher.h"
#include "lockfile.h"
#include "log.h"
#include "metadata.h"
#include "models.h"
#include "organize_exec.h"
#include "organize_plan.h"
#include "paths.h"
#include "relink.h"
#include "remove_exec.h"
#include "scan_engine.h"
#include "scanner.h"
#include "search_engine.h"
#include "selection.h"
#include "sidecar.h"
#include "thumbs.h"
#include "thumbs_engine.h"
#include "types.h"
#include "version.h"

namespace fs = std::filesystem;

namespace {
// The CLI installs a SIGINT handler that cancels whichever operation is
// running. A signal handler cannot take arguments, so the active token is
// reachable through this pointer. Only main.cpp touches it; the core takes a
// CancelToken by reference and knows nothing about signals.
std::atomic<CancelToken*> g_active_cancel{nullptr};

extern "C" void cli_sigint_handler(int) {
    CancelToken* t = g_active_cancel.load(std::memory_order_relaxed);
    if (t != nullptr) t->cancel();
}

// Installs the handler for the duration of the scope and restores it after.
class ScopedInterruptHandler {
   public:
    explicit ScopedInterruptHandler(CancelToken& token) {
        g_active_cancel.store(&token, std::memory_order_relaxed);
        prev_ = std::signal(SIGINT, cli_sigint_handler);
    }
    ~ScopedInterruptHandler() {
        std::signal(SIGINT, prev_);
        g_active_cancel.store(nullptr, std::memory_order_relaxed);
    }
    ScopedInterruptHandler(const ScopedInterruptHandler&) = delete;
    ScopedInterruptHandler& operator=(const ScopedInterruptHandler&) = delete;

   private:
    void (*prev_)(int) = nullptr;
};
}  // namespace

// Acquire the process-wide writer lock for a mutating command. Prints a clear
// message and exits nonzero if --no-wait was given and another op holds it.
static std::unique_ptr<WriterLock> acquire_writer_lock(bool no_wait) {
    std::string path = lock_path();
    fs::create_directories(fs::path(path).parent_path());
    try {
        return std::make_unique<WriterLock>(path, no_wait);
    } catch (const LockBusy& e) {
        std::cerr << "Error: " << e.what() << " (use without --no-wait to wait)\n";
        std::exit(1);
    }
}

// Full invocation string, stored on the operation row for the audit trail.
static std::string join_argv(int argc, char* argv[]) {
    std::string s;
    for (int i = 0; i < argc; i++) {
        if (i) s += ' ';
        s += argv[i];
    }
    return s;
}

// ---- Helpers ----

void print_usage() {
    std::cerr
        << "Usage:\n"
        << "  phig scan <directory> [flags]\n"
        << "  phig duplicates [flags]\n"
        << "  phig cp <destination> [flags]\n"
        << "  phig mv <destination> [flags]\n"
        << "  phig rm [--match|--exclude] [--hard|--db-only|--orphans] [--dry-run] [--yes]\n"
        << "                               Remove images: trash (default), --hard (permanent),\n"
        << "                               --db-only (keep file), --orphans (missing files)\n"
        << "  phig trash empty [--yes]     Permanently delete everything in the trash\n"
        << "  phig search [flags]\n"
        << "  phig face name <image> <name>     Name a face from a single-face photo\n"
        << "  phig face identify <name>          Label all matching faces in DB\n"
        << "  phig face rebuild [--match] [--exclude] [--parallel N]  Re-detect all faces\n"
        << "  phig face list                     List known people\n"
        << "  phig thumbs rebuild [flags]  Generate missing thumbnails\n"
        << "  phig thumbs clean            Remove orphaned thumbnails\n"
        << "  phig tag add|remove <tag> [--match|--exclude|--path]   Tag images\n"
        << "  phig tag list                                         List tags and counts\n"
        << "  phig gallery set <name>|clear [--match|--exclude|--path]  Assign/clear gallery\n"
        << "  phig gallery list                                     List galleries and counts\n"
        << "  phig fav add|remove [--match|--exclude|--path]         Favorite images\n"
        << "  phig fav list [output flags]                          List favorites\n"
        << "  phig models download\n"
        << "\nScan flags:\n"
        << "  --recursive              Scan subdirectories\n"
        << "  --on-error warn|fail     Behavior for unreadable files (default: warn)\n"
        << "  --force [what]           Re-process: hash,thumbs,faces,all (default: all)\n"
        << "  --faces                  Detect faces and compute embeddings\n"
        << "  --thumbs                 Generate thumbnails\n"
        << "  --ignore-mount-warning   Proceed even if all files are missing\n"
        << "  --media image|video|all  Restrict to one media type (default: all)\n"
        << "  --hidden                 Include hidden/temp files and directories (. or ~)\n"
        << "  --parallel N             Number of images to process in parallel (default: auto)\n"
        << "  --db <path>              Database path\n"
        << "\nDuplicates flags:\n"
        << "  --type exact|near|all    Duplicate type to find (default: all)\n"
        << "  --threshold [0-64]       Hamming distance for near duplicates (default: 5)\n"
        << "  --format text|csv|json   Output format (default: text)\n"
        << "  --output <path>          Write to file instead of stdout\n"
        << "  --match <glob>           Show groups containing matching files (repeatable, matches "
           "path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --media image|video|all  Filter by media type (default: all)\n"
        << "  --fav                    Only groups containing a favorite\n"
        << "  --db <path>              Database path\n"
        << "\nCopy/Move flags (cp, mv):\n"
        << "  --format <string>        Output path format (default: %Y/%m/%original)\n"
        << "                           Tokens: %Y %m %d %camera %make %gallery %fav %original\n"
        << "  --match <glob>           Only files matching glob (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --dry-run                Show what would happen without doing it\n"
        << "  --on-conflict skip|overwrite|rename  (default: skip)\n"
        << "  --db <path>              Database path\n"
        << "\nRemove flags (rm):\n"
        << "  --match <glob>           Remove entries matching glob (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --hard                   Delete the file from disk (permanent, no trash)\n"
        << "  --db-only                Remove the DB row only, keep the file on disk\n"
        << "  --orphans                Remove DB rows whose files are already missing\n"
        << "  --dry-run                Show what would be removed\n"
        << "  --yes                    Skip confirmation for broad removals\n"
        << "  --db <path>              Database path\n"
        << "\nSearch flags:\n"
        << "  --ext <extension>        Match file extension (e.g., jpg, png, cr2)\n"
        << "  --after <date>           Files dated after YYYY-MM-DD\n"
        << "  --before <date>          Files dated before YYYY-MM-DD\n"
        << "  --camera <string>        Substring match on EXIF camera model\n"
        << "  --make <string>          Substring match on EXIF camera make\n"
        << "  --min-size <size>        Minimum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
        << "  --max-size <size>        Maximum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
        << "  --media image|video|all  Filter by media type (default: all)\n"
        << "  --min-duration <dur>     Minimum video duration (e.g., 10s, 5m, 1h)\n"
        << "  --max-duration <dur>     Maximum video duration (e.g., 10s, 5m, 1h)\n"
        << "  --similar <image-path>   Find visually similar images (by phash)\n"
        << "  --similar-hash <hex>     Find images similar to this phash\n"
        << "  --threshold [0-64]       Hamming distance for --similar (default: 5)\n"
        << "  --face <image-path>      Find images containing this person\n"
        << "  --face-threshold <float> Distance threshold for face match (default: 0.6)\n"
        << "  --person <name>          Find images containing a named person\n"
        << "  --tag <name>             Images with this tag (repeatable, all must match)\n"
        << "  --gallery <name>         Images in this gallery\n"
        << "  --fav                    Favorited images (alias for --tag favorite)\n"
        << "  --blurry                 Low-sharpness images (blur_score <= 100)\n"
        << "  --min-sharpness <n>      Only images with blur_score >= n\n"
        << "  --max-sharpness <n>      Only images with blur_score <= n\n"
        << "  --match <glob>           Include matching files (repeatable, matches path)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable, wins over --match)\n"
        << "  --limit N                Maximum number of results\n"
        << "  --count                  Show count of matches only\n"
        << "  --porcelain              Tab-separated machine output\n"
        << "  --print0                 Null-separated paths\n"
        << "  --format text|csv|json   Output format (default: text)\n"
        << "  --output <path>          Write to file\n"
        << "  --db <path>              Database path\n"
        << "\nThumbs rebuild flags:\n"
        << "  --match <glob>           Only matching files (repeatable)\n"
        << "  --exclude <glob>          Exclude matching files (repeatable)\n"
        << "  --force                  Regenerate even if thumbnail exists\n"
        << "  --parallel N             Number of images to process in parallel (default: auto)\n"
        << "  --db <path>              Database path\n";
}

// ---- Scan command ----

ForceOptions parse_force(const std::string& value) {
    ForceOptions f;
    if (value.empty()) {
        f.set_all();
        return f;
    }
    std::istringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token == "hash")
            f.hash = true;
        else if (token == "thumbs")
            f.thumbs = true;
        else if (token == "faces")
            f.faces = true;
        else if (token == "all")
            f.set_all();
        else
            throw std::runtime_error("Unknown --force value: " + token +
                                     " (use hash,thumbs,faces,all)");
    }
    return f;
}

int cmd_scan(const ScanOptions& opts) {
    auto start_time = std::chrono::steady_clock::now();

    Database db(opts.db_path);
    db.init_schema();

    // Helper to show paths relative to the scan directory
    std::string base_dir = fs::canonical(opts.directory).string();
    if (base_dir.back() != '/') base_dir += '/';

    auto rel_path = [&](const fs::path& p) -> std::string {
        std::string full = p.string();
        if (full.starts_with(base_dir)) {
            return full.substr(base_dir.size());
        }
        return full;
    };

    std::cout << "Scanning: " << opts.directory << (opts.recursive ? " (recursive)" : "") << "\n";
    std::cout << "Database: " << opts.db_path << "\n";

    // Install signal handler for graceful Ctrl-C. Only covers ScanEngine::run:
    // it covers the engine's worker phase only. The guard is dropped before
    // run_reconcile, which deletes rows and polls no token, so a SIGINT there
    // aborts the process instead of being silently absorbed.
    CancelToken cancel;
    std::optional<ScopedInterruptHandler> interrupt_guard(cancel);

    // Everything the engine wants to show goes through the queue and is printed
    // here, on one thread, so a per-file warning can never be overwritten by
    // another worker's progress line. The engine runs on a worker thread and
    // this thread renders.
    ProgressQueue<ScanProgress> progress(1024);
    ScanEngine engine(opts, db);

    ScanResult result;
    std::exception_ptr engine_error;
    std::thread worker([&] {
        try {
            result = engine.run_workers(cancel, &progress);
        } catch (...) {
            engine_error = std::current_exception();
        }
        progress.close();
    });

    ScanProgress p;
    while (progress.pop(p)) {
        switch (p.stage) {
            case ScanStage::Walked:
                std::cout << "Found " << p.total << " files\n";
                break;
            case ScanStage::LoadingFaceModels:
                std::cout << "Loading face recognition models (" << p.total << " instances)...\n";
                break;
            case ScanStage::FileDone:
                if (!p.error.empty()) {
                    // Clear the progress line, then log the warning. Both happen
                    // on this thread with no progress print able to land in
                    // between, so the warning cannot be overwritten.
                    std::cout << "\r\033[K" << std::flush;
                    log_warn(p.error);
                }
                if (p.show_path) {
                    std::cout << "\r\033[K[" << p.done << "/" << p.total << "] " << rel_path(p.path)
                              << std::flush;
                } else {
                    std::cout << "\r[" << p.done << "/" << p.total << "] " << std::flush;
                }
                break;
        }
    }
    worker.join();

    // SAFETY: restore the plain SIGINT behavior before run_reconcile below, which
    // deletes user-authored metadata (tags, gallery, faces) for missing files. It
    // reads no CancelToken, so a signal during it must kill the process rather
    // than be swallowed by a token nothing there polls.
    interrupt_guard.reset();

    if (engine_error) {
        // --on-error fail: the engine flushed what completed and rethrew. Clear
        // the last progress line so the message below starts on a blank one.
        std::cout << "\r\033[K" << std::flush;
        try {
            std::rethrow_exception(engine_error);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
        }
        return 1;
    }

    if (!result.completed) {
        std::cout << "\n\nInterrupted. Progress saved.\n";
    }

    engine.run_reconcile(result);

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

    std::cout << "\n\nDone in " << secs << "s\n"
              << "  Processed: " << result.processed << " (" << result.images << " images, "
              << result.videos << " videos)\n"
              << "  Skipped:   " << result.skipped << "\n"
              << "  Errors:    " << result.errors << "\n"
              << "  Removed:   " << result.removed << "\n";
    if (result.relinked > 0) {
        std::cout << "  Relinked:  " << result.relinked
                  << " (metadata moved to identical files at new paths)\n";
    }
    if (result.demoted > 0) {
        std::cout << "  Demoted:   " << result.demoted
                  << " (metadata moved from JPEG to sibling RAW)\n";
    }
    if (opts.faces) {
        std::cout << "  Faces:     " << result.faces << "\n";
    }
    if (opts.thumbs) {
        std::cout << "  Thumbs:    " << result.thumbs << "\n";
    }
    std::cout << "  Total DB:  " << db.count() << " entries\n";

    return 0;
}

// ---- Duplicates command ----

struct DuplicatesOptions {
    std::string type = "all";     // exact, near, all
    int threshold = 5;            // hamming distance for near duplicates
    std::string format = "text";  // text, csv, json
    std::string output;           // empty = stdout
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    std::string media;  // "image", "video", or "" (all)
    bool fav = false;   // only groups containing at least one favorite
    std::string db_path;
};

void output_text(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                 const std::vector<DuplicateGroup>& near) {
    if (!exact.empty()) {
        out << "=== Exact Duplicates (same SHA256) ===\n\n";
        for (size_t g = 0; g < exact.size(); g++) {
            out << "Group " << (g + 1) << " (" << exact[g].images.size() << " files, "
                << exact[g].images[0].file_size << " bytes each):\n";
            for (const auto& img : exact[g].images) {
                out << "  " << img.path << "\n";
            }
            out << "\n";
        }
    }

    if (!near.empty()) {
        out << "=== Near Duplicates (similar phash) ===\n\n";
        for (size_t g = 0; g < near.size(); g++) {
            out << "Group " << (g + 1) << " (" << near[g].images.size() << " files):\n";
            for (const auto& img : near[g].images) {
                out << "  " << img.path << " [" << img.width << "x" << img.height << ", "
                    << img.file_size << " bytes]\n";
            }
            out << "\n";
        }
    }

    size_t exact_files = 0, near_files = 0;
    for (const auto& g : exact) exact_files += g.images.size();
    for (const auto& g : near) near_files += g.images.size();

    out << "Summary:\n"
        << "  Exact duplicate groups: " << exact.size() << " (" << exact_files << " files)\n"
        << "  Near duplicate groups:  " << near.size() << " (" << near_files << " files)\n";
}

void output_csv(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                const std::vector<DuplicateGroup>& near) {
    out << "group,type,path,filename,sha256,phash,width,height,file_size\n";
    int group_num = 0;

    for (const auto& g : exact) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",exact,\"" << img.path << "\"," << img.filename << ","
                << img.sha256 << "," << img.phash << "," << img.width << "," << img.height << ","
                << img.file_size << "\n";
        }
    }
    for (const auto& g : near) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",near,\"" << img.path << "\"," << img.filename << "," << img.sha256
                << "," << img.phash << "," << img.width << "," << img.height << "," << img.file_size
                << "\n";
        }
    }
}

void output_json(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                 const std::vector<DuplicateGroup>& near) {
    auto write_groups = [&](const std::vector<DuplicateGroup>& groups, bool& first_group) {
        for (const auto& g : groups) {
            if (!first_group) out << ",";
            first_group = false;
            out << "{\"type\":\"" << g.type << "\",\"files\":[";
            for (size_t i = 0; i < g.images.size(); i++) {
                if (i > 0) out << ",";
                const auto& img = g.images[i];
                out << "{\"path\":\"" << img.path << "\""
                    << ",\"filename\":\"" << img.filename << "\""
                    << ",\"sha256\":\"" << img.sha256 << "\""
                    << ",\"phash\":" << img.phash << ",\"width\":" << img.width
                    << ",\"height\":" << img.height << ",\"file_size\":" << img.file_size << "}";
            }
            out << "]}";
        }
    };

    out << "{\"groups\":[";
    bool first = true;
    write_groups(exact, first);
    write_groups(near, first);
    out << "]}\n";
}

// Select duplicate groups: keep only groups where at least one file passes selection
std::vector<DuplicateGroup> select_groups(const std::vector<DuplicateGroup>& groups,
                                          const std::vector<std::string>& matches,
                                          const std::vector<std::string>& excludes) {
    if (matches.empty() && excludes.empty()) return groups;

    std::vector<DuplicateGroup> result;
    for (const auto& g : groups) {
        bool has_match = false;
        for (const auto& img : g.images) {
            if (passes_selection(img.path, matches, excludes)) {
                has_match = true;
                break;
            }
        }
        if (has_match) result.push_back(g);
    }
    return result;
}

int cmd_duplicates(const DuplicatesOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    bool do_exact = (opts.type == "all" || opts.type == "exact");
    bool do_near = (opts.type == "all" || opts.type == "near");

    // Compare against full DB (within path prefix), then filter results
    std::vector<DuplicateGroup> exact_groups;
    std::set<std::string> exact_sha256s;

    if (do_exact) {
        auto groups = db.get_exact_duplicates();
        for (auto& g : groups) {
            exact_sha256s.insert(g[0].sha256);
            DuplicateGroup dg;
            dg.type = "exact";
            dg.distance = 0;
            dg.images = std::move(g);
            exact_groups.push_back(std::move(dg));
        }
        exact_groups = select_groups(exact_groups, opts.matches, opts.excludes);
        if (!opts.media.empty()) {
            exact_groups.erase(std::remove_if(exact_groups.begin(), exact_groups.end(),
                                              [&](const DuplicateGroup& g) {
                                                  return std::none_of(
                                                      g.images.begin(), g.images.end(),
                                                      [&](const ImageInfo& img) {
                                                          return img.media_type == opts.media;
                                                      });
                                              }),
                               exact_groups.end());
        }
    }

    std::vector<DuplicateGroup> near_groups;
    if (do_near) {
        auto all_images = db.get_all_images();
        std::cerr << "Comparing " << all_images.size() << " images for near duplicates...\n";
        near_groups = find_near_duplicates(all_images, opts.threshold, exact_sha256s);
        near_groups = select_groups(near_groups, opts.matches, opts.excludes);
        if (!opts.media.empty()) {
            near_groups.erase(std::remove_if(near_groups.begin(), near_groups.end(),
                                             [&](const DuplicateGroup& g) {
                                                 return std::none_of(
                                                     g.images.begin(), g.images.end(),
                                                     [&](const ImageInfo& img) {
                                                         return img.media_type == opts.media;
                                                     });
                                             }),
                              near_groups.end());
        }
    }

    // Keep only groups containing at least one favorited image.
    if (opts.fav) {
        Database::SearchCriteria fc;
        fc.tags = {"favorite"};
        std::set<std::string> fav_paths;
        for (const auto& img : db.search(fc)) {
            fav_paths.insert(img.path);
        }
        auto keep_favorited = [&](std::vector<DuplicateGroup>& groups) {
            groups.erase(std::remove_if(groups.begin(), groups.end(),
                                        [&](const DuplicateGroup& g) {
                                            return std::none_of(
                                                g.images.begin(), g.images.end(),
                                                [&](const ImageInfo& img) {
                                                    return fav_paths.count(img.path) > 0;
                                                });
                                        }),
                         groups.end());
        };
        keep_favorited(exact_groups);
        keep_favorited(near_groups);
    }

    // Output
    std::ofstream file_out;
    if (!opts.output.empty()) {
        file_out.open(opts.output);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << opts.output << "\n";
            return 1;
        }
    }
    std::ostream& out = opts.output.empty() ? std::cout : file_out;

    if (opts.format == "csv") {
        output_csv(out, exact_groups, near_groups);
    } else if (opts.format == "json") {
        output_json(out, exact_groups, near_groups);
    } else {
        output_text(out, exact_groups, near_groups);
    }

    return 0;
}

// ---- Organize command ----

struct OrganizeOptions {
    std::string destination;
    std::string format = "%Y/%m/%original";
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    bool is_move = false;  // true for mv, false for cp
    bool dry_run = false;
    std::string on_conflict = "skip";  // skip, overwrite, rename
    std::string db_path;
    bool log_enabled = true;
    int log_max_entries = 100000;
    std::string argv_str;  // full invocation, for the audit trail
};

int cmd_organize(const OrganizeOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    auto images = db.get_all_images();
    std::cout << "Found " << images.size() << " images in database\n";

    // Apply match/filter
    bool has_selection = !opts.matches.empty() || !opts.excludes.empty();
    if (has_selection) {
        images.erase(std::remove_if(images.begin(), images.end(),
                                    [&](const ImageInfo& img) {
                                        return !passes_selection(img.path, opts.matches,
                                                                 opts.excludes);
                                    }),
                     images.end());
        std::cout << "After match/filter: " << images.size() << " images\n";
    }

    // Plan moves/copies
    struct Action {
        std::string source;
        fs::path dest;
        bool skipped = false;
        std::string skip_reason;
    };
    std::vector<Action> actions;
    std::set<std::string> claimed;  // dest paths chosen this run, to catch intra-run collisions

    // All source paths in this batch. A destination equal to any OTHER image's
    // source is a real, irreplaceable original that hasn't moved yet — never
    // overwrite it (audit #1). A file may resolve to its own source (no-op).
    std::set<std::string> batch_sources;
    for (const auto& img : images) batch_sources.insert(img.path);

    for (const auto& img : images) {
        DateParts date = get_best_date(img);
        std::string rel_path = apply_format(opts.format, img, date);
        fs::path dest = fs::path(opts.destination) / rel_path;

        // Protect: (a) another image's source file in this batch, and (b) ANY
        // other real file on disk — tracked or not. Clobbering an untracked file
        // is silent data loss (audit #4); clobbering a tracked file destroys a
        // real image the DB still knows about and, with logging on, cascades its
        // faces/tags away with nothing in the log to restore them (plan 020
        // review). Overwriting only ever lands on a genuinely free path; existing
        // files are renamed around, so mv/cp never destroy irreplaceable bytes.
        const std::string& self_source = img.path;
        auto is_protected = [&](const std::string& path) -> bool {
            if (path == self_source) return false;
            if (batch_sources.count(path)) return true;
            if (fs::exists(path)) return true;
            return false;
        };

        auto resolved = resolve_conflict(dest.string(), opts.on_conflict, claimed, is_protected);
        Action act;
        act.source = img.path;
        if (resolved.empty()) {
            act.dest = dest;
            act.skipped = true;
            act.skip_reason = "already exists";
        } else {
            act.dest = resolved;
        }
        actions.push_back(std::move(act));
    }

    // Summary
    size_t to_process = 0, to_skip = 0;
    for (const auto& a : actions) {
        if (a.skipped)
            to_skip++;
        else
            to_process++;
    }

    std::string verb = opts.is_move ? "Move" : "Copy";
    std::cout << "\n"
              << verb << ": " << to_process << " files"
              << ", skip: " << to_skip << " (conflict/exists)\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& a : actions) {
            if (a.skipped) {
                std::cout << "  SKIP " << a.source << " → " << a.dest << " (" << a.skip_reason
                          << ")\n";
            } else {
                std::cout << "  " << verb << " " << a.source << " → " << a.dest << "\n";
            }
        }
        return 0;
    }

    // Execute. Handle SIGINT so Ctrl-C stops cleanly at a file boundary rather
    // than killing the process between a move and its DB update.
    CancelToken cancel;
    int64_t op_id = 0;
    const bool logging = opts.log_enabled;
    size_t done = 0, errs = 0;
    {
        // The handler covers log_begin_operation too, so a Ctrl-C there is
        // absorbed by the token and the run finishes as "interrupted" rather
        // than killing the process.
        ScopedInterruptHandler interrupt_guard(cancel);

        // Open a log operation for this run unless logging is disabled. Entries are
        // appended per file inside the same transaction as each DB change (plan 020).
        if (logging) {
            op_id =
                db.log_begin_operation(opts.is_move ? "mv" : "cp", opts.argv_str, iso_utc_now());
        }

        for (const auto& a : actions) {
            if (cancel.stop()) break;
            if (a.skipped) continue;

            try {
                // Disk op + DB update + optional log entry, committed together with
                // on-failure compensation so disk and DB never disagree (plan 020).
                execute_organize_action(db, a.source, a.dest.string(), opts.is_move,
                                        logging ? op_id : 0);
                done++;

                carry_sidecars(db, a.source, a.dest.string(), opts.is_move, logging ? op_id : 0);

                std::cout << "\r[" << done << "/" << to_process << "] " << std::flush;
            } catch (const std::exception& e) {
                std::cerr << "\nError: " << a.source << " → " << a.dest << ": " << e.what() << "\n";
                errs++;
            }
        }
    }

    if (logging) {
        db.log_finish_operation(op_id, cancel.stop() ? "interrupted" : "completed");
        db.log_prune(opts.log_max_entries);
    }

    std::cout << "\n\n";
    if (cancel.stop()) std::cout << "Interrupted. Stopped at a file boundary.\n";
    std::cout << "Done.\n"
              << "  Done: " << done << "\n"
              << "  Skipped: " << to_skip << "\n"
              << "  Errors: " << errs << "\n";

    return 0;
}

// ---- Remove command ----

int cmd_trash_empty(bool assume_yes) {
    std::string dir = trash_dir();
    if (!fs::exists(dir)) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        (void)e;
        n++;
    }
    if (n == 0) {
        std::cout << "Trash is empty.\n";
        return 0;
    }
    if (!assume_yes) {
        std::cout << "This will PERMANENTLY delete " << n << " item(s) from " << dir << ".\n";
        if (!::isatty(fileno(stdin))) {
            std::cerr << "Refusing without confirmation. Re-run with --yes.\n";
            return 1;
        }
        std::cout << "Type 'yes' to proceed: " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "yes") {
            std::cout << "Aborted.\n";
            return 1;
        }
    }
    for (const auto& e : fs::directory_iterator(dir)) fs::remove_all(e.path());
    std::cout << "Emptied trash (" << n << " item(s)).\n";
    return 0;
}

// ---- Search command ----

struct SearchOptions {
    std::string extension;
    std::string after;
    std::string before;
    std::string camera;
    std::string make;
    int64_t min_size = -1;
    int64_t max_size = -1;
    std::string media;  // "image", "video", or "" (all)
    int64_t min_duration_ms = -1;
    int64_t max_duration_ms = -1;
    std::string similar;       // image path for phash similarity
    std::string similar_hash;  // hex phash string
    int threshold = 5;
    std::string face;               // image path for face similarity search
    float face_threshold = 0.6;     // dlib recommended threshold
    std::string person;             // search by named person
    std::vector<std::string> tags;  // must have ALL these tags
    std::string gallery;            // exact gallery match
    double min_sharpness = -1;      // blur_score >= this
    double max_sharpness = -1;      // blur_score <= this
    std::vector<std::string> matches;
    std::vector<std::string> excludes;
    int limit = -1;
    bool count_only = false;
    bool porcelain = false;
    bool print0 = false;
    std::string format = "text";
    std::string output;
    std::string db_path;
};

namespace {

std::string format_size(int64_t bytes) {
    if (bytes >= static_cast<int64_t>(1024 * 1024)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0 * 1024.0));
        return buf;
    } else if (bytes >= 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fKB", bytes / 1024.0);
        return buf;
    }
    return std::to_string(bytes) + "B";
}

int64_t parse_size(const std::string& s) {
    if (s.empty()) throw std::runtime_error("Empty size value");

    // Find where the digits end
    size_t i = 0;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.')) i++;

    double num = std::stod(s.substr(0, i));
    std::string suffix = s.substr(i);

    // Lowercase the suffix
    for (auto& c : suffix) c = std::tolower(c);

    if (suffix.empty() || suffix == "b") return static_cast<int64_t>(num);
    if (suffix == "k" || suffix == "kb") return static_cast<int64_t>(num * 1024);
    if (suffix == "m" || suffix == "mb") return static_cast<int64_t>(num * 1024 * 1024);
    if (suffix == "g" || suffix == "gb") return static_cast<int64_t>(num * 1024 * 1024 * 1024);

    throw std::runtime_error("Unknown size suffix: " + suffix + " (use B, KB, MB, GB)");
}

// Parse a duration string like "10s", "5m", "1h", "1.5m" into milliseconds
int64_t parse_duration(const std::string& s) {
    if (s.empty()) throw std::runtime_error("Empty duration value");

    size_t i = 0;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.')) i++;

    double num = std::stod(s.substr(0, i));
    std::string suffix = s.substr(i);
    for (auto& c : suffix) c = std::tolower(c);

    if (suffix.empty() || suffix == "ms") return static_cast<int64_t>(num);
    if (suffix == "s") return static_cast<int64_t>(num * 1000);
    if (suffix == "m") return static_cast<int64_t>(num * 60 * 1000);
    if (suffix == "h") return static_cast<int64_t>(num * 3600 * 1000);

    throw std::runtime_error("Unknown duration suffix: " + suffix + " (use ms, s, m, h)");
}

// Read stdin to a temp file, return the path. Caller should delete when done.
std::string read_stdin_to_temp() {
    auto temp_path = fs::temp_directory_path() / "phig-stdin-image";
    std::ofstream out(temp_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Cannot create temp file: " + temp_path.string());
    }
    out << std::cin.rdbuf();
    out.close();
    if (fs::file_size(temp_path) == 0) {
        fs::remove(temp_path);
        throw std::runtime_error("No data received on stdin");
    }
    return temp_path.string();
}

uint64_t parse_hex_phash(const std::string& hex) {
    uint64_t val = 0;
    for (char c : hex) {
        val <<= 4;
        if (c >= '0' && c <= '9')
            val |= (c - '0');
        else if (c >= 'a' && c <= 'f')
            val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val |= (c - 'A' + 10);
        else
            throw std::runtime_error("Invalid hex character in phash: " + hex);
    }
    return val;
}

}  // anonymous namespace

int cmd_search(const SearchOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    bool is_similar = !opts.similar.empty() || !opts.similar_hash.empty();
    bool is_face_search = !opts.face.empty();
    bool is_person_search = !opts.person.empty();

    // Handle stdin ("-") for --face and --similar
    std::string temp_file;
    std::string face_path = opts.face;
    std::string similar_path = opts.similar;
    if (is_face_search && face_path == "-") {
        temp_file = read_stdin_to_temp();
        face_path = temp_file;
    } else if (is_similar && similar_path == "-") {
        temp_file = read_stdin_to_temp();
        similar_path = temp_file;
    }

    // Clean up temp file on exit
    struct TempCleanup {
        std::string path;
        ~TempCleanup() {
            if (!path.empty()) fs::remove(path);
        }
    } cleanup{temp_file};

    SearchQuery query;

    if (is_person_search) {
        query.mode = SearchMode::Person;
        query.person = opts.person;
    } else if (is_face_search) {
        // Face similarity search — decode with OpenCV, detect with dlib. The
        // engine gets only the resulting embeddings: it has no OpenCV
        // dependency and never touches the query file itself.
        preload_face_models();
        cv::Mat query_img = decode_image(face_path);
        if (query_img.empty()) {
            log_error("Cannot decode image: " + face_path);
            return 1;
        }
        auto faces = detect_faces(query_img);
        if (faces.empty()) {
            std::cerr << "No faces detected in: " << face_path << "\n";
            return 1;
        }
        std::cerr << "Detected " << faces.size() << " face(s), searching all...\n";

        query.mode = SearchMode::Face;
        for (const auto& face : faces) query.face_embeddings.push_back(face.embedding);
        query.face_threshold = opts.face_threshold;
        query.limit = opts.limit;
    } else if (is_similar) {
        query.mode = SearchMode::Similar;
        if (!similar_path.empty()) {
            query.phash = compute_phash(similar_path).hash;
        } else {
            query.phash = parse_hex_phash(opts.similar_hash);
        }
        query.phash_threshold = opts.threshold;
        query.matches = opts.matches;
        query.excludes = opts.excludes;
        query.limit = opts.limit;
    } else {
        query.mode = SearchMode::Filter;
        query.filter.extension = opts.extension;
        query.filter.after = opts.after;
        query.filter.before = opts.before;
        query.filter.camera = opts.camera;
        query.filter.make = opts.make;
        query.filter.min_size = opts.min_size;
        query.filter.max_size = opts.max_size;
        query.filter.media = opts.media;
        query.filter.min_duration_ms = opts.min_duration_ms;
        query.filter.max_duration_ms = opts.max_duration_ms;
        query.filter.tags = opts.tags;
        query.filter.gallery = opts.gallery;
        query.filter.min_sharpness = opts.min_sharpness;
        query.filter.max_sharpness = opts.max_sharpness;
        query.matches = opts.matches;
        query.excludes = opts.excludes;
        query.limit = opts.limit;
    }

    SearchEngine engine(query, db);
    SearchResult result = engine.run();

    if (is_person_search) {
        const auto& paths = result.paths;
        if (opts.count_only) {
            std::cout << paths.size() << "\n";
            return 0;
        }
        if (paths.empty()) {
            std::cout << "No images found for \"" << opts.person << "\"\n";
            return 0;
        }

        // Output
        std::ofstream file_out;
        if (!opts.output.empty()) {
            file_out.open(opts.output);
            if (!file_out) {
                std::cerr << "Error: cannot open output file\n";
                return 1;
            }
        }
        std::ostream& out = opts.output.empty() ? std::cout : file_out;

        if (opts.print0) {
            for (const auto& p : paths) out << p << '\0';
        } else if (opts.porcelain) {
            for (const auto& p : paths) out << p << '\n';
        } else {
            for (const auto& p : paths) out << p << "\n";
            out << "\nFound " << paths.size() << " images of \"" << opts.person << "\"\n";
        }
        return 0;
    }

    if (is_face_search) {
        const auto& face_results = result.matches;
        if (opts.count_only) {
            std::cout << face_results.size() << "\n";
            return 0;
        }

        // Convert to output — face search has its own output since it includes distance
        std::ofstream file_out;
        if (!opts.output.empty()) {
            file_out.open(opts.output);
            if (!file_out) {
                std::cerr << "Error: cannot open output file: " << opts.output << "\n";
                return 1;
            }
        }
        std::ostream& out = opts.output.empty() ? std::cout : file_out;

        if (opts.print0) {
            for (const auto& r : face_results) {
                out << r.path << '\0';
            }
        } else if (opts.porcelain) {
            for (const auto& r : face_results) {
                out << r.path << '\t' << r.distance << '\n';
            }
        } else if (opts.format == "json") {
            out << "{\"results\":[";
            for (size_t i = 0; i < face_results.size(); i++) {
                if (i > 0) out << ",";
                out << "{\"path\":\"" << face_results[i].path << "\""
                    << ",\"distance\":" << face_results[i].distance
                    << ",\"face_x\":" << face_results[i].face_x
                    << ",\"face_y\":" << face_results[i].face_y
                    << ",\"face_w\":" << face_results[i].face_w
                    << ",\"face_h\":" << face_results[i].face_h << "}";
            }
            out << "],\"count\":" << face_results.size() << "}\n";
        } else {
            for (const auto& r : face_results) {
                out << r.path << "\n"
                    << "  distance: " << r.distance << "  face: " << r.face_x << "," << r.face_y
                    << " " << r.face_w << "x" << r.face_h << "\n\n";
            }
            out << "Found " << face_results.size() << " matching faces\n";
        }
        return 0;
    }

    const auto& results = result.images;

    if (opts.count_only) {
        std::cout << results.size() << "\n";
        return 0;
    }

    // Output
    std::ofstream file_out;
    if (!opts.output.empty()) {
        file_out.open(opts.output);
        if (!file_out) {
            std::cerr << "Error: cannot open output file: " << opts.output << "\n";
            return 1;
        }
    }
    std::ostream& out = opts.output.empty() ? std::cout : file_out;

    if (opts.print0) {
        for (const auto& img : results) {
            out << img.path << '\0';
        }
    } else if (opts.porcelain) {
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << img.path << '\t' << img.width << '\t' << img.height << '\t' << img.file_size
                << '\t' << date << '\t' << camera << '\t' << tpath << '\n';
        }
    } else if (opts.format == "csv") {
        out << "path,filename,width,height,file_size,date,camera,sha256,phash,thumb_path\n";
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << "\"" << img.path << "\"," << img.filename << "," << img.width << ","
                << img.height << "," << img.file_size << "," << date << ","
                << "\"" << camera << "\"," << img.sha256 << "," << img.phash << ","
                << "\"" << tpath << "\"\n";
        }
    } else if (opts.format == "json") {
        out << "{\"results\":[";
        for (size_t i = 0; i < results.size(); i++) {
            if (i > 0) out << ",";
            const auto& img = results[i];
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            std::string tpath = img.sha256.size() >= 2 ? get_thumb_path(img.sha256) : "";
            out << "{\"path\":\"" << img.path << "\""
                << ",\"filename\":\"" << img.filename << "\""
                << ",\"width\":" << img.width << ",\"height\":" << img.height
                << ",\"file_size\":" << img.file_size << ",\"date\":\"" << date << "\""
                << ",\"camera\":\"" << camera << "\""
                << ",\"sha256\":\"" << img.sha256 << "\""
                << ",\"phash\":" << img.phash << ",\"thumb_path\":\"" << tpath << "\""
                << "}";
        }
        out << "],\"count\":" << results.size() << "}\n";
    } else {
        // Human-readable
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.metadata_json, "Model");
            out << img.path << "\n"
                << "  " << img.width << "x" << img.height << "  " << format_size(img.file_size)
                << "  " << date;
            if (!camera.empty()) out << "  " << camera;
            out << "\n\n";
        }
        out << "Found " << results.size() << " images\n";
    }

    return 0;
}

// ---- Thumbs command ----

int cmd_thumbs(int argc, char* argv[], const std::string& db_path, int default_parallel,
               bool no_wait) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig thumbs rebuild [flags]   Generate missing thumbnails\n"
                  << "  phig thumbs clean [--dry-run] Remove orphaned thumbnails\n";
        return 1;
    }

    std::string subcmd = argv[2];

    if (subcmd == "rebuild") {
        std::vector<std::string> matches, excludes;
        bool force = false;
        int parallel = default_parallel;

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--force") {
                force = true;
            } else if (arg == "--parallel" && i + 1 < argc) {
                parallel = std::stoi(argv[++i]);
            } else if (arg == "--db") {
                i++;  // already handled
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        auto lock = acquire_writer_lock(no_wait);

        auto start_time = std::chrono::steady_clock::now();

        Database db(db_path);
        db.init_schema();

        if (db.count() == 0) {
            std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
            return 1;
        }

        CancelToken cancel;
        ScopedInterruptHandler interrupt_guard(cancel);

        ThumbsOptions opts;
        opts.matches = matches;
        opts.excludes = excludes;
        opts.force = force;
        opts.parallel = parallel;

        // The engine runs on a worker thread and this thread renders, so a
        // per-file progress write can never be interleaved with the engine's
        // own state updates.
        ProgressQueue<ThumbsProgress> progress(1024);
        ThumbsEngine engine(opts, db);

        ThumbsResult result;
        std::exception_ptr engine_error;
        std::thread worker([&] {
            try {
                result = engine.run(cancel, &progress);
            } catch (...) {
                engine_error = std::current_exception();
            }
            progress.close();
        });

        ThumbsProgress p;
        while (progress.pop(p)) {
            switch (p.stage) {
                case ThumbsStage::Selected:
                    std::cout << "Generating thumbnails for " << p.total << " images...\n";
                    break;
                case ThumbsStage::FileDone:
                    if (p.show_path) {
                        fs::path path(p.path);
                        std::string display =
                            (path.parent_path().filename() / path.filename()).string();
                        std::cout << "\r\033[K[" << p.done << "/" << p.total << "] " << display
                                  << std::flush;
                    } else {
                        std::cout << "\r[" << p.done << "/" << p.total << "] " << std::flush;
                    }
                    break;
            }
        }
        worker.join();

        if (engine_error) {
            std::rethrow_exception(engine_error);
        }

        if (!result.completed) {
            std::cout << "\n\nInterrupted. Progress saved.\n";
        }

        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

        std::cout << "\n\nDone in " << secs << "s\n"
                  << "  Generated: " << result.generated << "\n"
                  << "  Skipped:   " << result.skipped << "\n"
                  << "  Errors:    " << result.errors << "\n";

        return 0;

    } else if (subcmd == "clean") {
        bool dry_run = false;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--dry-run") {
                dry_run = true;
            } else if (arg == "--db") {
                i++;  // already handled
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }

        auto lock = acquire_writer_lock(no_wait);

        Database db(db_path);
        db.init_schema();

        auto known_hashes = db.get_all_sha256s();
        std::string thumb_cache = thumbs_dir();

        if (!fs::exists(thumb_cache)) {
            std::cout << "No thumbnail cache directory found.\n";
            return 0;
        }

        size_t removed = 0;
        size_t kept = 0;
        size_t skipped_foreign = 0;  // files that don't match the thumbnail contract

        for (auto& entry : fs::recursive_directory_iterator(thumb_cache)) {
            if (!entry.is_regular_file()) continue;

            // Only ever touch files that match phig's thumbnail naming contract
            // (<64-hex-sha256>.jpg). Anything else in the cache dir — user files,
            // other apps' data if the dir is misconfigured — is left untouched.
            std::string name = entry.path().filename().string();
            if (!is_thumbnail_filename(name)) {
                skipped_foreign++;
                continue;
            }

            std::string stem = entry.path().stem().string();
            if (known_hashes.count(stem)) {
                kept++;
            } else if (dry_run) {
                std::cout << "  would remove " << entry.path().string() << "\n";
                removed++;
            } else {
                fs::remove(entry.path());
                removed++;
            }
        }

        // Remove empty subdirectories (skip in dry-run)
        if (!dry_run) {
            for (auto& entry : fs::directory_iterator(thumb_cache)) {
                if (entry.is_directory() && fs::is_empty(entry.path())) {
                    fs::remove(entry.path());
                }
            }
        }

        std::cout << (dry_run ? "Would remove " : "Removed ") << removed << " orphaned thumbnails\n"
                  << "Kept " << kept << " thumbnails\n";
        if (skipped_foreign > 0) {
            std::cout << "Left untouched " << skipped_foreign
                      << " non-thumbnail files in the cache directory\n";
        }

        return 0;

    } else {
        std::cerr << "Unknown thumbs subcommand: " << subcmd << "\n"
                  << "Usage:\n"
                  << "  phig thumbs rebuild [flags]   Generate missing thumbnails\n"
                  << "  phig thumbs clean [--dry-run] Remove orphaned thumbnails\n";
        return 1;
    }
}

// ---- Face command ----

int cmd_face(int argc, char* argv[], const std::string& db_path, int default_parallel,
             bool no_wait) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig face name <image> <name>     Name a face from a single-face photo\n"
                  << "  phig face identify <name>          Label all matching faces in DB\n"
                  << "  phig face rebuild [flags]          Re-detect all faces from scratch\n"
                  << "  phig face list                     List known people\n";
        return 1;
    }

    std::string subcmd = argv[2];
    std::unique_ptr<WriterLock> lock;
    if (subcmd == "name" || subcmd == "identify" || subcmd == "rebuild") {
        lock = acquire_writer_lock(no_wait);
    }
    Database db(db_path);
    db.init_schema();

    if (subcmd == "name") {
        if (argc < 5) {
            std::cerr << "Usage: phig face name <image> <name>\n";
            return 1;
        }
        std::string image_path = argv[3];
        std::string name = argv[4];

        // Detect faces in the reference image
        preload_face_models();
        cv::Mat img = decode_image(image_path);
        if (img.empty()) {
            log_error("Cannot decode image: " + image_path);
            return 1;
        }
        auto faces = detect_faces(img);

        if (faces.empty()) {
            std::cerr << "No faces detected in: " << image_path << "\n";
            return 1;
        }
        if (faces.size() > 1) {
            std::cerr << "Error: Multiple faces detected (" << faces.size()
                      << "). Please use a photo with a single face.\n";
            return 1;
        }

        // Find the closest matching face in the DB
        int64_t face_id = db.find_closest_face(faces[0].embedding, 0.6);
        if (face_id < 0) {
            // No matching face in DB — scan this image first
            std::cerr << "No matching face found in the database.\n"
                      << "Make sure the image (or similar photos) have been scanned with --faces "
                         "first.\n";
            return 1;
        }

        // Create or get person, assign to face
        int64_t person_id = db.get_or_create_person(name);
        db.set_face_person(face_id, person_id);

        std::cout << "Named face as \"" << name << "\"\n";
        return 0;

    } else if (subcmd == "identify") {
        if (argc < 4) {
            std::cerr << "Usage: phig face identify <name>\n";
            return 1;
        }
        std::string name = argv[3];

        // Get the person's reference embeddings (all faces already named as this person)
        auto people = db.list_people();
        int64_t person_id = -1;
        for (const auto& p : people) {
            if (p.name == name) {
                person_id = p.id;
                break;
            }
        }
        if (person_id < 0) {
            std::cerr << "Unknown person: " << name << "\n"
                      << "Use 'phig face name <image> <name>' first.\n";
            return 1;
        }

        // Get all face embeddings for this person (as reference)
        // Then search the entire DB for matching untagged faces
        auto all_images = db.get_all_images();
        std::cout << "Searching " << all_images.size() << " images for \"" << name << "\"...\n";

        // Get reference embeddings from already-named faces
        // We use the vec_faces table to find all faces near the named ones
        auto named_paths = db.get_images_for_person(name);
        if (named_paths.empty()) {
            std::cerr << "No reference faces found for \"" << name << "\".\n"
                      << "Use 'phig face name <image> <name>' to provide a reference.\n";
            return 1;
        }

        // Get embeddings of named faces
        // For each named face, search for similar untagged faces
        auto ref_rows = db.get_face_embeddings_for_person(person_id);
        std::vector<std::array<float, 128>> ref_embeddings;
        ref_embeddings.reserve(ref_rows.size());
        for (const auto& row : ref_rows) {
            ref_embeddings.push_back(row.embedding);
        }

        // Scan all untagged faces and check each against the reference embeddings.
        size_t labeled = 0;
        auto untagged_rows = db.get_unlabeled_face_embeddings();
        for (const auto& row : untagged_rows) {
            // Check against all reference embeddings
            for (const auto& ref_emb : ref_embeddings) {
                float dist = embedding_distance(ref_emb, row.embedding);
                if (dist <= 0.6) {
                    db.set_face_person(row.face_id, person_id);
                    labeled++;
                    break;
                }
            }
        }

        std::cout << "Labeled " << labeled << " additional faces as \"" << name << "\"\n";
        return 0;

    } else if (subcmd == "rebuild") {
        // Parse match/filter flags
        FacesRebuildOptions opts;
        int parallel = default_parallel;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--parallel" && i + 1 < argc) {
                parallel = std::stoi(argv[++i]);
            } else if (arg == "--db") {
                i++;  // already handled
            }
        }
        opts.parallel = parallel;

        CancelToken cancel;
        ScopedInterruptHandler interrupt_guard(cancel);

        // The engine runs on a worker thread and this thread renders, so a
        // per-file progress write can never be interleaved with the engine's
        // own state updates.
        ProgressQueue<FacesProgress> progress(1024);
        FacesEngine engine(opts, db);

        FacesResult result;
        std::exception_ptr engine_error;
        std::thread worker([&] {
            try {
                result = engine.run(cancel, &progress);
            } catch (...) {
                engine_error = std::current_exception();
            }
            progress.close();
        });

        FacesProgress p;
        while (progress.pop(p)) {
            switch (p.stage) {
                case FacesStage::Selected:
                    if (p.total == 0) {
                        std::cout << "No matching images found.\n";
                    } else {
                        std::cout << "Rebuilding face data for " << p.total << " images...\n";
                    }
                    break;
                case FacesStage::LoadingFaceModels:
                    std::cout << "Loading face recognition models (" << p.total
                              << " instances)...\n";
                    break;
                case FacesStage::FileDone: {
                    fs::path path(p.path);
                    std::string display =
                        (path.parent_path().filename() / path.filename()).string();
                    std::cout << "\r\033[K[" << p.done << "/" << p.total << "] " << display
                              << std::flush;
                    break;
                }
            }
        }
        worker.join();

        if (engine_error) {
            std::rethrow_exception(engine_error);
        }

        // "No matching images found." was already printed above from the
        // Selected update (p.total == 0); stop here so nothing below it prints.
        if (result.total_selected == 0) {
            return 0;
        }

        if (!result.completed) {
            std::cout << "\n\nInterrupted. Progress saved.\n";
        }

        std::cout << "\n\nDone. " << result.faces_detected << " faces detected.\n";
        return 0;

    } else if (subcmd == "list") {
        auto people = db.list_people();
        if (people.empty()) {
            std::cout << "No named people.\n";
            return 0;
        }
        for (const auto& p : people) {
            std::cout << p.name << " (" << p.face_count << " faces)\n";
        }
        return 0;

    } else {
        std::cerr << "Unknown face subcommand: " << subcmd << "\n";
        return 1;
    }
}

// ---- Main ----

// Resolve the images selected by an optional --path prefix plus match/filter
// globs into a list of paths, reusing the shared filter logic.
static std::vector<std::string> select_paths(Database& db, const std::string& path_prefix,
                                             const std::vector<std::string>& matches,
                                             const std::vector<std::string>& excludes) {
    std::vector<std::string> paths;
    for (const auto& img : db.get_all_images(path_prefix)) {
        if (passes_selection(img.path, matches, excludes)) {
            paths.push_back(img.path);
        }
    }
    return paths;
}

int cmd_tag(int argc, char* argv[], const std::string& db_path, bool no_wait) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig tag add <tag> [--match|--exclude|--path]\n"
                  << "  phig tag remove <tag> [--match|--exclude|--path]\n"
                  << "  phig tag list\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "list") {
        Database db(db_path);
        db.init_schema();
        for (const auto& [name, count] : db.get_tag_counts()) {
            std::cout << name << " (" << count << ")\n";
        }
        return 0;
    }

    if (subcmd == "add" || subcmd == "remove") {
        if (argc < 4 || argv[3][0] == '-') {
            std::cerr << "Error: tag " << subcmd << " requires a <tag> argument\n";
            return 1;
        }
        std::string tag = argv[3];
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        for (int i = 4; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: tag " << subcmd << " requires --match or --path\n";
            return 1;
        }
        auto lock = acquire_writer_lock(no_wait);
        Database db(db_path);
        db.init_schema();
        int n = db.set_tag(select_paths(db, path_prefix, matches, excludes), tag, subcmd == "add");
        std::cout << (subcmd == "add" ? "Tagged " : "Untagged ") << n << " images (" << tag
                  << ")\n";
        return 0;
    }

    std::cerr << "Unknown tag subcommand: " << subcmd << "\n";
    return 1;
}

int cmd_gallery(int argc, char* argv[], const std::string& db_path, bool no_wait) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig gallery set <name> [--match|--exclude|--path] [--no-overwrite]\n"
                  << "  phig gallery clear [--match|--exclude|--path]\n"
                  << "  phig gallery list\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "list") {
        Database db(db_path);
        db.init_schema();
        for (const auto& [name, count] : db.get_gallery_counts()) {
            std::cout << name << " (" << count << ")\n";
        }
        return 0;
    }

    if (subcmd == "set" || subcmd == "clear") {
        bool is_set = subcmd == "set";
        std::string gallery;
        int first_flag = 3;
        if (is_set) {
            if (argc < 4 || argv[3][0] == '-') {
                std::cerr << "Error: gallery set requires a <name> argument\n";
                return 1;
            }
            gallery = argv[3];
            first_flag = 4;
        }
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        bool no_overwrite = false;
        for (int i = first_flag; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--no-overwrite" && is_set) {
                no_overwrite = true;
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: gallery " << subcmd << " requires --match or --path\n";
            return 1;
        }
        auto lock = acquire_writer_lock(no_wait);
        Database db(db_path);
        db.init_schema();
        auto paths = select_paths(db, path_prefix, matches, excludes);
        if (is_set) {
            int n = db.set_gallery(paths, gallery, no_overwrite);
            std::cout << "Set gallery '" << gallery << "' on " << n << " images\n";
        } else {
            int n = db.clear_gallery(paths);
            std::cout << "Cleared gallery on " << n << " images\n";
        }
        return 0;
    }

    std::cerr << "Unknown gallery subcommand: " << subcmd << "\n";
    return 1;
}

// Favorites are the built-in `favorite` tag; these commands are ergonomic sugar.
int cmd_fav(int argc, char* argv[], const std::string& db_path, bool no_wait) {
    if (argc < 3) {
        std::cerr
            << "Usage:\n"
            << "  phig fav add [--match|--exclude|--path]\n"
            << "  phig fav remove [--match|--exclude|--path]\n"
            << "  phig fav list [--match|--exclude] [--format|--porcelain|--print0|--output]\n";
        return 1;
    }
    std::string subcmd = argv[2];

    if (subcmd == "add" || subcmd == "remove") {
        std::string path_prefix;
        std::vector<std::string> matches, excludes;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--path" && i + 1 < argc) {
                path_prefix = expand_tilde(argv[++i]);
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        if (matches.empty() && path_prefix.empty()) {
            std::cerr << "Error: fav " << subcmd << " requires --match or --path\n";
            return 1;
        }
        auto lock = acquire_writer_lock(no_wait);
        Database db(db_path);
        db.init_schema();
        int n = db.set_tag(select_paths(db, path_prefix, matches, excludes), "favorite",
                           subcmd == "add");
        std::cout << (subcmd == "add" ? "Favorited " : "Unfavorited ") << n << " images\n";
        return 0;
    }

    if (subcmd == "list") {
        // Delegate to search on the favorite tag so all output modes are reused.
        SearchOptions opts;
        opts.db_path = db_path;
        opts.tags.push_back("favorite");
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--limit" && i + 1 < argc) {
                opts.limit = std::stoi(argv[++i]);
            } else if (arg == "--count") {
                opts.count_only = true;
            } else if (arg == "--porcelain") {
                opts.porcelain = true;
            } else if (arg == "--print0") {
                opts.print0 = true;
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--db" && i + 1 < argc) {
                i++;  // resolved by caller
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        return cmd_search(opts);
    }

    std::cerr << "Unknown fav subcommand: " << subcmd << "\n";
    return 1;
}

// Print the effective configuration (resolved paths + any config overrides),
// shown at the top of --help output.
static void print_active_config(const Config& cfg, const std::string& cfg_path, bool loaded) {
    std::cout << "Active configuration:\n";
    std::cout << "  config file:  " << cfg_path << (loaded ? "  (loaded)" : "  (not found)")
              << "\n";
    std::cout << "  db:           " << default_db_path() << (cfg.db ? "  [config]" : "  (default)")
              << "\n";
    std::cout << "  models_dir:   " << models_dir()
              << (cfg.models_dir ? "  [config]" : "  (default)") << "\n";
    std::cout << "  thumbs_dir:   " << thumbs_dir()
              << (cfg.thumbs_dir ? "  [config]" : "  (default)") << "\n";

    // Non-path values are listed only when set in the config file.
    auto line = [](const char* k, const std::string& v) {
        std::cout << "  " << k << " = " << v << "  [config]\n";
    };
    if (cfg.parallel) line("general.parallel", std::to_string(*cfg.parallel));
    if (cfg.scan_recursive) line("scan.recursive", *cfg.scan_recursive ? "true" : "false");
    if (cfg.scan_on_error) line("scan.on_error", *cfg.scan_on_error);
    if (cfg.scan_media) line("scan.media", *cfg.scan_media);
    if (cfg.scan_thumbs) line("scan.thumbs", *cfg.scan_thumbs ? "true" : "false");
    if (cfg.scan_faces) line("scan.faces", *cfg.scan_faces ? "true" : "false");
    if (cfg.organize_format) line("organize.format", *cfg.organize_format);
    if (cfg.duplicates_type) line("duplicates.type", *cfg.duplicates_type);
    if (cfg.duplicates_threshold)
        line("duplicates.threshold", std::to_string(*cfg.duplicates_threshold));
    if (cfg.faces_threshold) line("faces.threshold", std::to_string(*cfg.faces_threshold));
    std::cout << "\n";
}

int main(int argc, char* argv[]) {
    // Extract an optional global --config <path> from anywhere in the args, then
    // continue with it removed so the per-command parsers don't see it.
    std::string config_file;
    bool have_config_file = false;
    bool no_wait = false;
    std::vector<char*> filtered_args;
    filtered_args.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) {
            config_file = argv[++i];
            have_config_file = true;
        } else if (a == "--no-wait") {
            no_wait = true;
        } else {
            filtered_args.push_back(argv[i]);
        }
    }
    argv = filtered_args.data();
    argc = static_cast<int>(filtered_args.size());

    // --version works regardless of config state.
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") {
            std::cout << "phig " << PHIG_VERSION << "\n";
            return 0;
        }
    }

    // Load configuration (missing file -> defaults). Install path overrides so
    // default_db_path()/models_dir()/thumbs_dir() reflect the config everywhere.
    std::string resolved_cfg_path = have_config_file ? config_file : config_path();
    Config cfg;
    try {
        cfg = load_config(resolved_cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    set_path_overrides(cfg.db, cfg.models_dir, cfg.thumbs_dir);

    // Diagnostic logger (plan 024): stderr sink for warnings/errors plus a
    // rotating file sink at ~/.local/state/phig/phig.log unless disabled.
    // Init before any dispatch so every instrumented site has a live logger.
    log_init(default_log_path(), !cfg.log_file || *cfg.log_file,
             cfg.log_file_level ? log_level_from_string(*cfg.log_file_level) : LogLevel::Info);

    bool cfg_loaded = fs::exists(resolved_cfg_path);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        print_active_config(cfg, resolved_cfg_path, cfg_loaded);
        print_usage();
        return 0;
    }

    // Check for --help on any command
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_active_config(cfg, resolved_cfg_path, cfg_loaded);
            print_usage();
            return 0;
        }
    }

    log_info(join_argv(argc, argv));  // file-only: the stderr sink is warn+

    if (command == "scan") {
        if (argc < 3) {
            std::cerr << "Error: scan requires a directory argument\n";
            print_usage();
            return 1;
        }

        ScanOptions opts;
        opts.directory = argv[2];
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.parallel) opts.parallel = *cfg.parallel;
        if (cfg.scan_recursive) opts.recursive = *cfg.scan_recursive;
        if (cfg.scan_thumbs) opts.thumbs = *cfg.scan_thumbs;
        if (cfg.scan_faces) opts.faces = *cfg.scan_faces;
        if (cfg.scan_on_error) opts.fail_on_error = (*cfg.scan_on_error == "fail");
        if (cfg.scan_media) opts.media = (*cfg.scan_media == "all") ? "" : *cfg.scan_media;
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--recursive") {
                opts.recursive = true;
            } else if (arg == "--force") {
                // --force with optional value: --force or --force hash,thumbs
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    auto f = parse_force(argv[++i]);
                    opts.force.hash = opts.force.hash || f.hash;
                    opts.force.thumbs = opts.force.thumbs || f.thumbs;
                    opts.force.faces = opts.force.faces || f.faces;
                } else {
                    opts.force.set_all();
                }
            } else if (arg == "--faces") {
                opts.faces = true;
            } else if (arg == "--thumbs") {
                opts.thumbs = true;
            } else if (arg == "--ignore-mount-warning") {
                opts.ignore_mount_warning = true;
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--hidden") {
                opts.include_hidden = true;
            } else if (arg == "--parallel" && i + 1 < argc) {
                opts.parallel = std::stoi(argv[++i]);
            } else if (arg == "--on-error" && i + 1 < argc) {
                std::string val = argv[++i];
                if (val == "fail") {
                    opts.fail_on_error = true;
                } else if (val == "warn") {
                    opts.fail_on_error = false;
                } else {
                    std::cerr << "Error: --on-error must be 'warn' or 'fail'\n";
                    return 1;
                }
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            auto lock = acquire_writer_lock(no_wait);
            return cmd_scan(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "duplicates") {
        DuplicatesOptions opts;
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.duplicates_type) opts.type = *cfg.duplicates_type;
        if (cfg.duplicates_threshold) opts.threshold = *cfg.duplicates_threshold;

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--type" && i + 1 < argc) {
                opts.type = argv[++i];
                if (opts.type != "exact" && opts.type != "near" && opts.type != "all") {
                    std::cerr << "Error: --type must be 'exact', 'near', or 'all'\n";
                    return 1;
                }
            } else if (arg == "--threshold" && i + 1 < argc) {
                opts.threshold = std::stoi(argv[++i]);
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
                if (opts.format != "text" && opts.format != "csv" && opts.format != "json") {
                    std::cerr << "Error: --format must be 'text', 'csv', or 'json'\n";
                    return 1;
                }
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--fav") {
                opts.fav = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_duplicates(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "cp" || command == "mv") {
        if (argc < 3) {
            std::cerr << "Error: " << command << " requires a destination directory\n";
            print_usage();
            return 1;
        }

        OrganizeOptions opts;
        opts.destination = argv[2];
        opts.is_move = (command == "mv");
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.organize_format) opts.format = *cfg.organize_format;
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--on-conflict" && i + 1 < argc) {
                opts.on_conflict = argv[++i];
                if (opts.on_conflict != "skip" && opts.on_conflict != "overwrite" &&
                    opts.on_conflict != "rename") {
                    std::cerr << "Error: --on-conflict must be 'skip', 'overwrite', or 'rename'\n";
                    return 1;
                }
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            auto lock = acquire_writer_lock(no_wait);
            return cmd_organize(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "rm") {
        RemoveOptions opts;
        opts.db_path = default_db_path();
        if (cfg.log_enabled) opts.log_enabled = *cfg.log_enabled;
        if (cfg.log_max_entries) opts.log_max_entries = *cfg.log_max_entries;
        opts.argv_str = join_argv(argc, argv);

        int mode_flags = 0;
        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--hard") {
                opts.mode = RemoveMode::Hard;
                mode_flags++;
            } else if (arg == "--db-only") {
                opts.mode = RemoveMode::DbOnly;
                mode_flags++;
            } else if (arg == "--orphans") {
                opts.mode = RemoveMode::Orphans;
                mode_flags++;
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--yes" || arg == "-y") {
                opts.assume_yes = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }
        if (mode_flags > 1) {
            std::cerr << "Error: --hard, --db-only, and --orphans are mutually exclusive\n";
            return 1;
        }
        ConfirmFn confirm = [](const std::string& prompt) {
            std::cout << prompt;
            if (!::isatty(fileno(stdin))) {
                std::cerr << "Refusing without confirmation.\n"
                             "Re-run with --yes to proceed, or --dry-run to preview.\n";
                return false;
            }
            std::cout << "Type 'yes' to proceed: " << std::flush;
            std::string answer;
            std::getline(std::cin, answer);
            if (answer != "yes") {
                std::cout << "Aborted.\n";
                return false;
            }
            return true;
        };
        try {
            auto lock = acquire_writer_lock(no_wait);
            return cmd_rm(opts, confirm);
        } catch (const std::exception& e) {
            log_error(std::string("rm: ") + e.what());
            return 1;
        }
    } else if (command == "trash") {
        // Only subcommand: `trash empty`.
        if (argc < 3 || std::string(argv[2]) != "empty") {
            std::cerr << "Usage: phig trash empty [--yes]\n";
            return 1;
        }
        bool yes = false;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--yes" || arg == "-y")
                yes = true;
            else {
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        }
        try {
            auto lock = acquire_writer_lock(no_wait);
            return cmd_trash_empty(yes);
        } catch (const std::exception& e) {
            log_error(std::string("trash: ") + e.what());
            return 1;
        }
    } else if (command == "thumbs") {
        std::string thumbs_db = default_db_path();
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                thumbs_db = argv[i + 1];
                break;
            }
        }
        try {
            return cmd_thumbs(argc, argv, thumbs_db, cfg.parallel.value_or(-1), no_wait);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "tag" || command == "gallery" || command == "fav") {
        std::string db_path = default_db_path();
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                db_path = argv[i + 1];
                break;
            }
        }
        try {
            if (command == "tag") return cmd_tag(argc, argv, db_path, no_wait);
            if (command == "gallery") return cmd_gallery(argc, argv, db_path, no_wait);
            return cmd_fav(argc, argv, db_path, no_wait);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "face") {
        std::string face_db = default_db_path();
        // Check for --db flag
        for (int i = 2; i < argc; i++) {
            if (std::string(argv[i]) == "--db" && i + 1 < argc) {
                face_db = argv[i + 1];
                break;
            }
        }
        try {
            return cmd_face(argc, argv, face_db, cfg.parallel.value_or(-1), no_wait);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "models") {
        if (argc < 3 || std::string(argv[2]) != "download") {
            std::cerr << "Usage: phig models download\n";
            return 1;
        }
        try {
            auto lock = acquire_writer_lock(no_wait);
            if (download_face_models()) {
                std::cout << "All models downloaded successfully.\n";
                return 0;
            } else {
                std::cerr << "Model download failed.\n";
                return 1;
            }
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else if (command == "search") {
        SearchOptions opts;
        opts.db_path = default_db_path();
        // Seed from config; CLI flags below override.
        if (cfg.faces_threshold) opts.face_threshold = static_cast<float>(*cfg.faces_threshold);

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--ext" && i + 1 < argc) {
                opts.extension = argv[++i];
            } else if (arg == "--after" && i + 1 < argc) {
                opts.after = argv[++i];
            } else if (arg == "--before" && i + 1 < argc) {
                opts.before = argv[++i];
            } else if (arg == "--camera" && i + 1 < argc) {
                opts.camera = argv[++i];
            } else if (arg == "--make" && i + 1 < argc) {
                opts.make = argv[++i];
            } else if (arg == "--min-size" && i + 1 < argc) {
                opts.min_size = parse_size(argv[++i]);
            } else if (arg == "--max-size" && i + 1 < argc) {
                opts.max_size = parse_size(argv[++i]);
            } else if (arg == "--similar" && i + 1 < argc) {
                opts.similar = argv[++i];
            } else if (arg == "--similar-hash" && i + 1 < argc) {
                opts.similar_hash = argv[++i];
            } else if (arg == "--threshold" && i + 1 < argc) {
                opts.threshold = std::stoi(argv[++i]);
            } else if (arg == "--face" && i + 1 < argc) {
                opts.face = argv[++i];
            } else if (arg == "--face-threshold" && i + 1 < argc) {
                opts.face_threshold = std::stof(argv[++i]);
            } else if (arg == "--person" && i + 1 < argc) {
                opts.person = argv[++i];
            } else if (arg == "--tag" && i + 1 < argc) {
                opts.tags.push_back(argv[++i]);
            } else if (arg == "--gallery" && i + 1 < argc) {
                opts.gallery = argv[++i];
            } else if (arg == "--fav") {
                opts.tags.push_back("favorite");
            } else if (arg == "--min-sharpness" && i + 1 < argc) {
                opts.min_sharpness = std::stod(argv[++i]);
            } else if (arg == "--max-sharpness" && i + 1 < argc) {
                opts.max_sharpness = std::stod(argv[++i]);
            } else if (arg == "--blurry") {
                if (opts.max_sharpness < 0) opts.max_sharpness = 100.0;  // rough default
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--exclude" && i + 1 < argc) {
                opts.excludes.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--limit" && i + 1 < argc) {
                opts.limit = std::stoi(argv[++i]);
            } else if (arg == "--count") {
                opts.count_only = true;
            } else if (arg == "--porcelain") {
                opts.porcelain = true;
            } else if (arg == "--print0") {
                opts.print0 = true;
            } else if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
                if (opts.format != "text" && opts.format != "csv" && opts.format != "json") {
                    std::cerr << "Error: --format must be 'text', 'csv', or 'json'\n";
                    return 1;
                }
            } else if (arg == "--media" && i + 1 < argc) {
                opts.media = argv[++i];
                if (opts.media != "image" && opts.media != "video" && opts.media != "all") {
                    std::cerr << "Error: --media must be 'image', 'video', or 'all'\n";
                    return 1;
                }
                if (opts.media == "all") opts.media = "";
            } else if (arg == "--min-duration" && i + 1 < argc) {
                try {
                    opts.min_duration_ms = parse_duration(argv[++i]);
                } catch (const std::exception& e) {
                    log_error(command + ": invalid --min-duration: " + e.what());
                    return 1;
                }
            } else if (arg == "--max-duration" && i + 1 < argc) {
                try {
                    opts.max_duration_ms = parse_duration(argv[++i]);
                } catch (const std::exception& e) {
                    log_error(command + ": invalid --max-duration: " + e.what());
                    return 1;
                }
            } else if (arg == "--output" && i + 1 < argc) {
                opts.output = argv[++i];
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_search(opts);
        } catch (const std::exception& e) {
            log_error(command + ": " + e.what());
            return 1;
        }
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }
}
