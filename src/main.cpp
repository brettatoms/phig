#include "scanner.h"
#include "hasher.h"
#include "exif.h"
#include "database.h"
#include "types.h"
#include "filters.h"
#include "faces.h"
#include "models.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <bit>
#include <functional>
#include <csignal>

namespace fs = std::filesystem;

// Graceful shutdown on Ctrl-C
static volatile std::sig_atomic_t g_interrupted = 0;
static void sigint_handler(int) { g_interrupted = 1; }

// ---- Helpers ----

std::string default_db_path() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            throw std::runtime_error("Cannot determine home directory");
        }
        base = std::string(home) + "/.local/share";
    }
    return base + "/phig/phig.db";
}

std::string format_time(fs::file_time_type ftime) {
    auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
        std::chrono::clock_cast<std::chrono::system_clock>(ftime));
    auto time_t = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm;
    gmtime_r(&time_t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

void print_usage() {
    std::cerr << "Usage:\n"
              << "  phig scan <directory> [flags]\n"
              << "  phig duplicates [flags]\n"
              << "  phig cp <destination> [flags]\n"
              << "  phig mv <destination> [flags]\n"
              << "  phig purge [flags]\n"
              << "  phig search [flags]\n"
              << "  phig face name <image> <name>     Name a face from a single-face photo\n"
              << "  phig face identify <name>          Label all matching faces in DB\n"
              << "  phig face rebuild [--match] [--filter] [--parallel N]  Re-detect all faces\n"
              << "  phig face list                     List known people\n"
              << "  phig models download\n"
              << "\nScan flags:\n"
              << "  --recursive              Scan subdirectories\n"
              << "  --on-error warn|fail     Behavior for unreadable files (default: warn)\n"
              << "  --force                  Re-hash all files, ignore mtime check\n"
              << "  --faces                  Detect faces and compute embeddings\n"
              << "  --parallel N             Number of images to process in parallel (default: auto)\n"
              << "  --db <path>              Database path\n"
              << "\nDuplicates flags:\n"
              << "  --type exact|near|all    Duplicate type to find (default: all)\n"
              << "  --threshold [0-64]       Hamming distance for near duplicates (default: 5)\n"
              << "  --format text|csv|json   Output format (default: text)\n"
              << "  --output <path>          Write to file instead of stdout\n"
              << "  --match <glob>           Show groups containing matching files (repeatable, matches path)\n"
              << "  --filter <glob>          Exclude matching files (repeatable, wins over --match)\n"
              << "  --db <path>              Database path\n"
              << "\nCopy/Move flags (cp, mv):\n"
              << "  --format <string>        Output path format (default: %Y/%m/%original)\n"
              << "                           Tokens: %Y %m %d %camera %make %original\n"
              << "  --match <glob>           Only files matching glob (repeatable, matches path)\n"
              << "  --filter <glob>          Exclude matching files (repeatable, wins over --match)\n"
              << "  --dry-run                Show what would happen without doing it\n"
              << "  --on-conflict skip|overwrite|rename  (default: skip)\n"
              << "  --db <path>              Database path\n"
              << "\nPurge flags:\n"
              << "  --match <glob>           Remove entries matching glob (repeatable, matches path)\n"
              << "  --filter <glob>          Exclude matching files (repeatable, wins over --match)\n"
              << "  --dry-run                Show what would be removed\n"
              << "  --db <path>              Database path\n"
              << "\nSearch flags:\n"
              << "  --ext <extension>        Match file extension (e.g., jpg, png, cr2)\n"
              << "  --after <date>           Files dated after YYYY-MM-DD\n"
              << "  --before <date>          Files dated before YYYY-MM-DD\n"
              << "  --camera <string>        Substring match on EXIF camera model\n"
              << "  --make <string>          Substring match on EXIF camera make\n"
              << "  --min-size <size>        Minimum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
              << "  --max-size <size>        Maximum file size (e.g., 1000, 5KB, 10MB, 1GB)\n"
              << "  --similar <image-path>   Find visually similar images (by phash)\n"
              << "  --similar-hash <hex>     Find images similar to this phash\n"
              << "  --threshold [0-64]       Hamming distance for --similar (default: 5)\n"
              << "  --face <image-path>      Find images containing this person\n"
              << "  --face-threshold <float> Distance threshold for face match (default: 0.6)\n"
              << "  --person <name>          Find images containing a named person\n"
              << "  --match <glob>           Include filter (repeatable, matches path)\n"
              << "  --filter <glob>          Exclude matching files (repeatable, wins over --match)\n"
              << "  --limit N                Maximum number of results\n"
              << "  --count                  Show count of matches only\n"
              << "  --porcelain              Tab-separated machine output\n"
              << "  --print0                 Null-separated paths\n"
              << "  --format text|csv|json   Output format (default: text)\n"
              << "  --output <path>          Write to file\n"
              << "  --db <path>              Database path\n";
}

// ---- Scan command ----

struct ScanOptions {
    std::string directory;
    bool recursive = false;
    bool fail_on_error = false;
    bool force = false;
    bool faces = false;
    int parallel = -1;  // -1 = auto
    std::string db_path;
};

struct ProcessResult {
    ImageInfo info;
    std::vector<FaceInfo> faces;  // empty if faces not requested
};

ProcessResult process_file(const fs::path& filepath, FaceDetector* face_detector) {
    ProcessResult result;
    auto& info = result.info;

    info.path = fs::canonical(filepath).string();
    info.filename = filepath.filename().string();
    info.extension = filepath.extension().string();
    if (!info.extension.empty() && info.extension[0] == '.') {
        info.extension = info.extension.substr(1);
    }
    // Lowercase extension
    std::transform(info.extension.begin(), info.extension.end(),
                   info.extension.begin(), ::tolower);

    // File metadata
    auto status = fs::status(filepath);
    info.file_size = fs::file_size(filepath);
    info.modified_at = format_time(fs::last_write_time(filepath));

    // SHA256
    info.sha256 = compute_sha256(info.path);

    // Decode image once
    cv::Mat img = decode_image(info.path);
    if (img.empty()) {
        throw std::runtime_error("Cannot decode image: " + info.path);
    }

    // Phash from decoded image
    auto phash_result = compute_phash(img);
    info.phash = phash_result.hash;
    info.width = phash_result.width;
    info.height = phash_result.height;

    // Face detection from same decoded image (no re-decode)
    if (face_detector) {
        result.faces = face_detector->detect(img);
    }

    // EXIF
    info.exif_json = extract_exif_json(info.path);

    return result;
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

    std::cout << "Scanning: " << opts.directory
              << (opts.recursive ? " (recursive)" : "") << "\n";
    std::cout << "Database: " << opts.db_path << "\n";

    // Collect files
    auto files = scan_directory(opts.directory, opts.recursive);
    std::cout << "Found " << files.size() << " files\n";

    // Determine thread count
    int num_threads;
    if (opts.parallel > 0) {
        num_threads = opts.parallel;
    } else if (opts.faces) {
        // With faces: conservative default (each thread loads ~150MB of models)
        num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
        num_threads = std::max(num_threads, 1);
    } else {
        num_threads = std::max(1u, std::thread::hardware_concurrency());
    }

    // Create per-thread face detectors if needed
    std::vector<std::unique_ptr<FaceDetector>> face_detectors;
    if (opts.faces) {
        std::cout << "Loading face recognition models (" << num_threads << " instances)...\n";
        for (int i = 0; i < num_threads; i++) {
            face_detectors.push_back(std::make_unique<FaceDetector>());
        }
    }

    // Install signal handler for graceful Ctrl-C
    g_interrupted = 0;
    auto prev_handler = std::signal(SIGINT, sigint_handler);

    // Process files with thread pool
    std::atomic<size_t> processed{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> errors{0};
    std::atomic<size_t> face_count{0};

    if (!files.empty()) {
    std::atomic<size_t> next_file{0};

    std::mutex db_mutex;
    std::vector<ProcessResult> batch;
    const size_t batch_size = 10; // small batches so Ctrl-C loses less work

    auto flush_batch = [&]() {
        if (batch.empty()) return;
        // Insert image metadata
        std::vector<ImageInfo> infos;
        for (auto& r : batch) infos.push_back(r.info);
        db.insert_batch(infos);

        // Insert faces (need image_id from DB) and auto-label known people
        for (auto& r : batch) {
            if (!r.faces.empty()) {
                int64_t image_id = db.get_image_id(r.info.path);
                if (image_id >= 0) {
                    for (const auto& face : r.faces) {
                        db.insert_face(image_id, face);
                        int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                        db.auto_label_face(face_id, face.embedding);
                    }
                    face_count.fetch_add(r.faces.size());
                }
            }
        }
        batch.clear();
    };

    auto worker = [&](int thread_id) {
        FaceDetector* detector = (!face_detectors.empty()) ? face_detectors[thread_id].get() : nullptr;
        while (!g_interrupted) {
            size_t idx = next_file.fetch_add(1);
            if (idx >= files.size()) break;

            const auto& filepath = files[idx];
            std::string canonical;
            try {
                canonical = fs::canonical(filepath).string();
            } catch (...) {
                errors.fetch_add(1);
                continue;
            }

            // Check if we should skip (path + mtime match)
            bool needs_full_process = opts.force;
            bool needs_faces_only = false;

            if (!opts.force) {
                std::string mtime;
                try {
                    mtime = format_time(fs::last_write_time(filepath));
                } catch (...) {
                    // Can't stat, will fail later
                }

                std::lock_guard<std::mutex> lock(db_mutex);
                auto existing_mtime = db.get_modified_at(canonical);
                if (existing_mtime && *existing_mtime == mtime) {
                    // File unchanged — check if we only need faces
                    if (detector && !db.has_faces(canonical)) {
                        needs_faces_only = true;
                    } else {
                        skipped.fetch_add(1);
                        size_t done = processed.load() + skipped.load() + errors.load();
                        std::cout << "\r[" << done << "/" << files.size() << "] " << std::flush;
                        continue;
                    }
                } else {
                    needs_full_process = true;
                }
            }

            try {
                if (needs_faces_only) {
                    // Only decode and detect faces — skip hash/phash/EXIF
                    cv::Mat img = decode_image(canonical);
                    if (!img.empty() && detector) {
                        auto faces = detector->detect(img);
                        if (!faces.empty()) {
                            std::lock_guard<std::mutex> lock(db_mutex);
                            // Flush any pending batch first, then insert faces in a transaction
                            flush_batch();
                            db.begin_transaction();
                            int64_t image_id = db.get_image_id(canonical);
                            if (image_id >= 0) {
                                for (const auto& face : faces) {
                                    db.insert_face(image_id, face);
                                    int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                                    db.auto_label_face(face_id, face.embedding);
                                }
                                face_count.fetch_add(faces.size());
                            }
                            db.commit_transaction();
                        }
                    }
                    processed.fetch_add(1);
                } else {
                    auto result = process_file(filepath, detector);

                    std::lock_guard<std::mutex> lock(db_mutex);
                    batch.push_back(std::move(result));
                    if (batch.size() >= batch_size) {
                        flush_batch();
                    }
                    processed.fetch_add(1);
                }
            } catch (const std::exception& e) {
                errors.fetch_add(1);
                if (opts.fail_on_error) {
                    std::cerr << "\n[error] " << rel_path(filepath) << " -- " << e.what() << "\n";
                    throw;
                } else {
                    std::cerr << "\n[warning] " << rel_path(filepath) << " -- " << e.what() << "\n";
                }
            }

            size_t done = processed.load() + skipped.load() + errors.load();
            std::cout << "\r\033[K[" << done << "/" << files.size() << "] "
                      << rel_path(filepath) << std::flush;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Flush remaining
    flush_batch();
    } // end if (!files.empty())

    // Restore previous signal handler
    std::signal(SIGINT, prev_handler);

    if (g_interrupted) {
        std::cout << "\n\nInterrupted. Progress saved.\n";
    }

    // Clean up entries for files that no longer exist on disk
    std::string scan_prefix = fs::canonical(opts.directory).string();
    if (scan_prefix.back() != '/') scan_prefix += '/';

    auto db_paths = db.get_paths_with_prefix(scan_prefix);
    std::vector<std::string> removed_paths;
    for (const auto& p : db_paths) {
        if (!fs::exists(p)) {
            removed_paths.push_back(p);
        }
    }
    size_t removed_count = 0;
    if (!removed_paths.empty()) {
        if (!db_paths.empty() && removed_paths.size() == db_paths.size() && !opts.force) {
            std::cerr << "\nWarning: All " << removed_paths.size()
                      << " previously scanned files in this directory are missing.\n"
                      << "This may indicate an unmounted drive or incorrect path.\n"
                      << "Use --force to remove these entries from the database.\n";
        } else {
            db.delete_paths(removed_paths);
            removed_count = removed_paths.size();
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;

    std::cout << "\n\nDone in " << secs << "s\n"
              << "  Processed: " << processed.load() << "\n"
              << "  Skipped:   " << skipped.load() << "\n"
              << "  Errors:    " << errors.load() << "\n"
              << "  Removed:   " << removed_count << "\n";
    if (opts.faces) {
        std::cout << "  Faces:     " << face_count.load() << "\n";
    }
    std::cout << "  Total DB:  " << db.count() << " images\n";

    return 0;
}

// ---- Duplicates command ----

struct DuplicatesOptions {
    std::string type = "all";      // exact, near, all
    int threshold = 5;             // hamming distance for near duplicates
    std::string format = "text";   // text, csv, json
    std::string output;            // empty = stdout
    std::vector<std::string> matches;
    std::vector<std::string> filters;
    std::string db_path;
};

int hamming_distance(uint64_t a, uint64_t b) {
    return std::popcount(a ^ b);
}

struct DuplicateGroup {
    std::string type; // "exact" or "near"
    int distance;     // 0 for exact
    std::vector<ImageInfo> images;
};

std::vector<DuplicateGroup> find_near_duplicates(
    const std::vector<ImageInfo>& all_images,
    int threshold,
    const std::set<std::string>& exact_sha256s)
{
    std::vector<DuplicateGroup> groups;

    // Union-find to group near duplicates
    std::vector<int> parent(all_images.size());
    std::vector<int> min_dist(all_images.size(), 0);
    for (size_t i = 0; i < parent.size(); i++) parent[i] = i;

    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };

    auto unite = [&](int a, int b) {
        a = find(a); b = find(b);
        if (a != b) parent[a] = b;
    };

    for (size_t i = 0; i < all_images.size(); i++) {
        for (size_t j = i + 1; j < all_images.size(); j++) {
            // Skip pairs that are exact duplicates
            if (all_images[i].sha256 == all_images[j].sha256 &&
                exact_sha256s.count(all_images[i].sha256)) {
                continue;
            }
            int dist = hamming_distance(all_images[i].phash, all_images[j].phash);
            if (dist <= threshold) {
                unite(i, j);
            }
        }
    }

    // Collect groups
    std::map<int, std::vector<size_t>> group_map;
    for (size_t i = 0; i < all_images.size(); i++) {
        group_map[find(i)].push_back(i);
    }

    for (const auto& [root, indices] : group_map) {
        if (indices.size() < 2) continue;
        DuplicateGroup g;
        g.type = "near";
        g.distance = threshold;
        for (size_t idx : indices) {
            g.images.push_back(all_images[idx]);
        }
        groups.push_back(std::move(g));
    }

    return groups;
}

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
                out << "  " << img.path
                    << " [" << img.width << "x" << img.height << ", "
                    << img.file_size << " bytes]\n";
            }
            out << "\n";
        }
    }

    size_t exact_files = 0, near_files = 0;
    for (const auto& g : exact) exact_files += g.images.size();
    for (const auto& g : near) near_files += g.images.size();

    out << "Summary:\n"
        << "  Exact duplicate groups: " << exact.size()
        << " (" << exact_files << " files)\n"
        << "  Near duplicate groups:  " << near.size()
        << " (" << near_files << " files)\n";
}

void output_csv(std::ostream& out, const std::vector<DuplicateGroup>& exact,
                const std::vector<DuplicateGroup>& near) {
    out << "group,type,path,filename,sha256,phash,width,height,file_size\n";
    int group_num = 0;

    for (const auto& g : exact) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",exact,\"" << img.path << "\","
                << img.filename << "," << img.sha256 << ","
                << img.phash << "," << img.width << "," << img.height << ","
                << img.file_size << "\n";
        }
    }
    for (const auto& g : near) {
        group_num++;
        for (const auto& img : g.images) {
            out << group_num << ",near,\"" << img.path << "\","
                << img.filename << "," << img.sha256 << ","
                << img.phash << "," << img.width << "," << img.height << ","
                << img.file_size << "\n";
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
                    << ",\"phash\":" << img.phash
                    << ",\"width\":" << img.width
                    << ",\"height\":" << img.height
                    << ",\"file_size\":" << img.file_size << "}";
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

// Filter duplicate groups: keep only groups where at least one file passes filters
std::vector<DuplicateGroup> filter_groups(
    const std::vector<DuplicateGroup>& groups,
    const std::vector<std::string>& matches,
    const std::vector<std::string>& filters)
{
    if (matches.empty() && filters.empty()) return groups;

    std::vector<DuplicateGroup> result;
    for (const auto& g : groups) {
        bool has_match = false;
        for (const auto& img : g.images) {
            if (passes_filters(img.path, matches, filters)) {
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
        exact_groups = filter_groups(exact_groups, opts.matches, opts.filters);
    }

    std::vector<DuplicateGroup> near_groups;
    if (do_near) {
        auto all_images = db.get_all_images();
        std::cerr << "Comparing " << all_images.size() << " images for near duplicates...\n";
        near_groups = find_near_duplicates(all_images, opts.threshold, exact_sha256s);
        near_groups = filter_groups(near_groups, opts.matches, opts.filters);
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
    std::vector<std::string> filters;
    bool is_move = false;           // true for mv, false for cp
    bool dry_run = false;
    std::string on_conflict = "skip"; // skip, overwrite, rename
    std::string db_path;
};

namespace {

// Parse a date string from EXIF or mtime, return components
struct DateParts {
    std::string year = "unknown";
    std::string month = "unknown";
    std::string day = "unknown";
    bool valid = false;
};

DateParts parse_date(const std::string& date_str) {
    DateParts d;
    if (date_str.empty()) return d;

    // Try EXIF format: "YYYY:MM:DD HH:MM:SS"
    if (date_str.size() >= 10 && date_str[4] == ':' && date_str[7] == ':') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    // Try ISO format: "YYYY-MM-DDTHH:MM:SSZ"
    if (date_str.size() >= 10 && date_str[4] == '-' && date_str[7] == '-') {
        d.year = date_str.substr(0, 4);
        d.month = date_str.substr(5, 2);
        d.day = date_str.substr(8, 2);
        d.valid = (d.year != "0000" && d.year != "1970");
        return d;
    }

    return d;
}

// Extract a JSON string value by key (simple parser, no nesting)
std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

DateParts get_best_date(const ImageInfo& img) {
    // 1. EXIF DateTimeOriginal
    std::string dt = json_get(img.exif_json, "DateTimeOriginal");
    DateParts d = parse_date(dt);
    if (d.valid) return d;

    // 2. EXIF CreateDate / DateTimeDigitized
    dt = json_get(img.exif_json, "DateTimeDigitized");
    d = parse_date(dt);
    if (d.valid) return d;

    // 3. File modified time
    d = parse_date(img.modified_at);
    if (d.valid) return d;

    return d; // invalid — will go to unsorted
}

std::string apply_format(const std::string& fmt, const ImageInfo& img, const DateParts& date) {
    std::string result;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
            // Check for multi-char tokens first
            if (fmt.substr(i, 9) == "%original") {
                result += img.filename;
                i += 8;
            } else if (fmt.substr(i, 7) == "%camera") {
                std::string cam = json_get(img.exif_json, "Model");
                result += cam.empty() ? "unknown-camera" : cam;
                i += 6;
            } else if (fmt.substr(i, 5) == "%make") {
                std::string make = json_get(img.exif_json, "Make");
                result += make.empty() ? "unknown-make" : make;
                i += 4;
            } else if (fmt[i + 1] == 'Y') {
                result += date.valid ? date.year : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'm') {
                result += date.valid ? date.month : "unsorted";
                i += 1;
            } else if (fmt[i + 1] == 'd') {
                result += date.valid ? date.day : "unsorted";
                i += 1;
            } else {
                result += fmt[i]; // unknown token, keep as-is
            }
        } else {
            result += fmt[i];
        }
    }
    return result;
}

fs::path resolve_conflict(const fs::path& dest, const std::string& on_conflict) {
    if (!fs::exists(dest)) return dest;

    if (on_conflict == "overwrite") return dest;
    if (on_conflict == "skip") return ""; // empty = skip

    // rename: append numeric suffix
    auto stem = dest.stem().string();
    auto ext = dest.extension().string();
    auto parent = dest.parent_path();
    for (int i = 1; i < 10000; i++) {
        auto candidate = parent / (stem + "_" + std::to_string(i) + ext);
        if (!fs::exists(candidate)) return candidate;
    }

    return ""; // give up
}

} // anonymous namespace

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
    bool has_filters = !opts.matches.empty() || !opts.filters.empty();
    if (has_filters) {
        images.erase(
            std::remove_if(images.begin(), images.end(),
                [&](const ImageInfo& img) {
                    return !passes_filters(img.path, opts.matches, opts.filters);
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

    for (const auto& img : images) {
        DateParts date = get_best_date(img);
        std::string rel_path = apply_format(opts.format, img, date);
        fs::path dest = fs::path(opts.destination) / rel_path;

        auto resolved = resolve_conflict(dest, opts.on_conflict);
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
        if (a.skipped) to_skip++;
        else to_process++;
    }

    std::string verb = opts.is_move ? "Move" : "Copy";
    std::cout << "\n" << verb << ": " << to_process << " files"
              << ", skip: " << to_skip << " (conflict/exists)\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& a : actions) {
            if (a.skipped) {
                std::cout << "  SKIP " << a.source << " → " << a.dest
                          << " (" << a.skip_reason << ")\n";
            } else {
                std::cout << "  " << verb << " " << a.source << " → " << a.dest << "\n";
            }
        }
        return 0;
    }

    // Execute
    size_t done = 0, errs = 0;
    for (const auto& a : actions) {
        if (a.skipped) continue;

        try {
            fs::create_directories(a.dest.parent_path());

            if (opts.is_move) {
                fs::rename(a.source, a.dest);
                // Update DB — file has moved
                db.update_path(a.source, a.dest.string());
            } else {
                fs::copy_file(a.source, a.dest, fs::copy_options::overwrite_existing);
                // Don't update DB — source still exists at original path
            }
            done++;

            std::cout << "\r[" << done << "/" << to_process << "] " << std::flush;
        } catch (const std::exception& e) {
            std::cerr << "\nError: " << a.source << " → " << a.dest
                      << ": " << e.what() << "\n";
            errs++;
        }
    }

    std::cout << "\n\nDone.\n"
              << "  Done: " << done << "\n"
              << "  Skipped: " << to_skip << "\n"
              << "  Errors: " << errs << "\n";

    return 0;
}

// ---- Purge command ----

struct PurgeOptions {
    std::vector<std::string> matches;
    std::vector<std::string> filters;
    bool dry_run = false;
    std::string db_path;
};

int cmd_purge(const PurgeOptions& opts) {
    if (opts.matches.empty()) {
        std::cerr << "Error: purge requires at least one --match pattern\n";
        return 1;
    }

    Database db(opts.db_path);

    auto all_images = db.get_all_images();

    // Apply match/filter
    std::vector<std::string> matched;
    for (const auto& img : all_images) {
        if (passes_filters(img.path, opts.matches, opts.filters)) {
            matched.push_back(img.path);
        }
    }

    if (matched.empty()) {
        std::cout << "No matching entries found.\n";
        return 0;
    }

    std::cout << "Found " << matched.size() << " matching entries\n";

    if (opts.dry_run) {
        std::cout << "\n[DRY RUN]\n";
        for (const auto& p : matched) {
            std::cout << "  " << p << "\n";
        }
        return 0;
    }

    db.delete_paths(matched);
    std::cout << "Removed " << matched.size() << " entries from database.\n"
              << "Total DB: " << db.count() << " images\n";
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
    std::string similar;          // image path for phash similarity
    std::string similar_hash;     // hex phash string
    int threshold = 5;
    std::string face;             // image path for face similarity search
    float face_threshold = 0.6;   // dlib recommended threshold
    std::string person;           // search by named person
    std::vector<std::string> matches;
    std::vector<std::string> filters;
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
    if (bytes >= 1024 * 1024) {
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

std::string get_best_date_str(const ImageInfo& img) {
    // Try EXIF DateTimeOriginal
    std::string dt = json_get(img.exif_json, "DateTimeOriginal");
    if (!dt.empty() && dt.size() >= 10 && dt.substr(0, 4) != "0000" && dt.substr(0, 4) != "1970") {
        return dt.substr(0, 10);
    }
    // Try DateTimeDigitized
    dt = json_get(img.exif_json, "DateTimeDigitized");
    if (!dt.empty() && dt.size() >= 10 && dt.substr(0, 4) != "0000" && dt.substr(0, 4) != "1970") {
        return dt.substr(0, 10);
    }
    // Fall back to modified_at
    if (!img.modified_at.empty() && img.modified_at.size() >= 10) {
        return img.modified_at.substr(0, 10);
    }
    return "unknown";
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
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else throw std::runtime_error("Invalid hex character in phash: " + hex);
    }
    return val;
}

} // anonymous namespace

int cmd_search(const SearchOptions& opts) {
    Database db(opts.db_path);
    db.init_schema();

    if (db.count() == 0) {
        std::cerr << "Database is empty. Run 'phig scan <directory>' first.\n";
        return 1;
    }

    std::vector<ImageInfo> results;
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
        ~TempCleanup() { if (!path.empty()) fs::remove(path); }
    } cleanup{temp_file};

    if (is_person_search) {
        // Search by named person
        auto paths = db.get_images_for_person(opts.person);
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
            if (!file_out) { std::cerr << "Error: cannot open output file\n"; return 1; }
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
    } else if (is_face_search) {
        // Face similarity search — decode with OpenCV, detect with dlib
        preload_face_models();
        cv::Mat query_img = decode_image(face_path);
        if (query_img.empty()) {
            std::cerr << "Cannot decode image: " << face_path << "\n";
            return 1;
        }
        auto faces = detect_faces(query_img);
        if (faces.empty()) {
            std::cerr << "No faces detected in: " << face_path << "\n";
            return 1;
        }

        std::cerr << "Detected " << faces.size() << " face(s), searching all...\n";

        // Search for ALL faces found in the query image
        std::vector<Database::FaceSearchResult> face_results;
        std::set<std::string> seen_paths;
        int per_face_limit = opts.limit > 0 ? opts.limit : 100;

        for (const auto& face : faces) {
            auto matches = db.search_faces(face.embedding, opts.face_threshold, per_face_limit);
            for (auto& m : matches) {
                // Deduplicate by path — keep the closest match per image
                if (seen_paths.insert(m.path).second) {
                    face_results.push_back(std::move(m));
                }
            }
        }

        // Sort by distance
        std::sort(face_results.begin(), face_results.end(),
            [](const Database::FaceSearchResult& a, const Database::FaceSearchResult& b) {
                return a.distance < b.distance;
            });

        if (opts.limit > 0 && static_cast<int>(face_results.size()) > opts.limit) {
            face_results.resize(opts.limit);
        }

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
                    << "  distance: " << r.distance
                    << "  face: " << r.face_x << "," << r.face_y
                    << " " << r.face_w << "x" << r.face_h << "\n\n";
            }
            out << "Found " << face_results.size() << " matching faces\n";
        }
        return 0;
    } else if (is_similar) {
        // Compute or parse the query phash
        uint64_t query_phash;
        if (!similar_path.empty()) {
            auto phash_result = compute_phash(similar_path);
            query_phash = phash_result.hash;
        } else {
            query_phash = parse_hex_phash(opts.similar_hash);
        }

        // Get all images (within path prefix) and compare
        auto all_images = db.get_all_images();

        // Compute distances and filter
        struct ScoredImage {
            ImageInfo info;
            int distance;
        };
        std::vector<ScoredImage> scored;

        for (auto& img : all_images) {
            int dist = std::popcount(query_phash ^ img.phash);
            if (dist <= opts.threshold) {
                scored.push_back({std::move(img), dist});
            }
        }

        // Sort by distance (closest first)
        std::sort(scored.begin(), scored.end(),
            [](const ScoredImage& a, const ScoredImage& b) {
                return a.distance < b.distance;
            });

        for (auto& s : scored) {
            results.push_back(std::move(s.info));
        }
    } else {
        // Build search criteria
        Database::SearchCriteria criteria;
        criteria.extension = opts.extension;
        criteria.after = opts.after;
        criteria.before = opts.before;
        criteria.camera = opts.camera;
        criteria.make = opts.make;
        criteria.min_size = opts.min_size;
        criteria.max_size = opts.max_size;
        // Don't apply limit in DB if we also need match/filter (apply after)
        if (opts.matches.empty() && opts.filters.empty()) {
            criteria.limit = opts.limit;
        }
        results = db.search(criteria);
    }

    // Apply match/filter
    if (!opts.matches.empty() || !opts.filters.empty()) {
        results.erase(
            std::remove_if(results.begin(), results.end(),
                [&](const ImageInfo& img) {
                    return !passes_filters(img.path, opts.matches, opts.filters);
                }),
            results.end());
    }

    // Apply limit (if not already applied in DB)
    if (opts.limit > 0 && static_cast<int>(results.size()) > opts.limit) {
        results.resize(opts.limit);
    }

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
            std::string camera = json_get(img.exif_json, "Model");
            out << img.path << '\t'
                << img.width << '\t' << img.height << '\t'
                << img.file_size << '\t'
                << date << '\t'
                << camera << '\n';
        }
    } else if (opts.format == "csv") {
        out << "path,filename,width,height,file_size,date,camera,sha256,phash\n";
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.exif_json, "Model");
            out << "\"" << img.path << "\"," << img.filename << ","
                << img.width << "," << img.height << ","
                << img.file_size << "," << date << ","
                << "\"" << camera << "\"," << img.sha256 << ","
                << img.phash << "\n";
        }
    } else if (opts.format == "json") {
        out << "{\"results\":[";
        for (size_t i = 0; i < results.size(); i++) {
            if (i > 0) out << ",";
            const auto& img = results[i];
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.exif_json, "Model");
            out << "{\"path\":\"" << img.path << "\""
                << ",\"filename\":\"" << img.filename << "\""
                << ",\"width\":" << img.width
                << ",\"height\":" << img.height
                << ",\"file_size\":" << img.file_size
                << ",\"date\":\"" << date << "\""
                << ",\"camera\":\"" << camera << "\""
                << ",\"sha256\":\"" << img.sha256 << "\""
                << ",\"phash\":" << img.phash << "}";
        }
        out << "],\"count\":" << results.size() << "}\n";
    } else {
        // Human-readable
        for (const auto& img : results) {
            std::string date = get_best_date_str(img);
            std::string camera = json_get(img.exif_json, "Model");
            out << img.path << "\n"
                << "  " << img.width << "x" << img.height
                << "  " << format_size(img.file_size)
                << "  " << date;
            if (!camera.empty()) out << "  " << camera;
            out << "\n\n";
        }
        out << "Found " << results.size() << " images\n";
    }

    return 0;
}

// ---- Face command ----

int cmd_face(int argc, char* argv[], const std::string& db_path) {
    if (argc < 3) {
        std::cerr << "Usage:\n"
                  << "  phig face name <image> <name>     Name a face from a single-face photo\n"
                  << "  phig face identify <name>          Label all matching faces in DB\n"
                  << "  phig face rebuild [flags]          Re-detect all faces from scratch\n"
                  << "  phig face list                     List known people\n";
        return 1;
    }

    std::string subcmd = argv[2];
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
            std::cerr << "Cannot decode image: " << image_path << "\n";
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
                      << "Make sure the image (or similar photos) have been scanned with --faces first.\n";
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
            if (p.name == name) { person_id = p.id; break; }
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
        const char* ref_sql = R"(
            SELECT f.id, f.embedding FROM faces f
            WHERE f.person_id = ?
        )";
        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db.db_ptr(), ref_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Error querying reference faces\n";
            return 1;
        }
        sqlite3_bind_int64(stmt, 1, person_id);

        std::vector<std::array<float, 128>> ref_embeddings;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::array<float, 128> emb;
            const float* data = static_cast<const float*>(sqlite3_column_blob(stmt, 1));
            if (data) {
                std::copy(data, data + 128, emb.begin());
                ref_embeddings.push_back(emb);
            }
        }
        sqlite3_finalize(stmt);

        // For each reference embedding, find all matching faces and label them
        size_t labeled = 0;
        for (const auto& ref_emb : ref_embeddings) {
            auto matches = db.search_faces(ref_emb, 0.6, 1000);
            for (const auto& match : matches) {
                // Find the face_id for this match and label it
                int64_t fid = db.find_closest_face(ref_emb, 0.6);
                // Actually we need all matching face IDs — let's query directly
            }
        }

        // Simpler approach: scan all untagged faces and check against references
        const char* untagged_sql = "SELECT id, embedding FROM faces WHERE person_id IS NULL";
        rc = sqlite3_prepare_v2(db.db_ptr(), untagged_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Error querying faces\n";
            return 1;
        }

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t face_id = sqlite3_column_int64(stmt, 0);
            const float* data = static_cast<const float*>(sqlite3_column_blob(stmt, 1));
            if (!data) continue;

            std::array<float, 128> emb;
            std::copy(data, data + 128, emb.begin());

            // Check against all reference embeddings
            for (const auto& ref_emb : ref_embeddings) {
                float dist = embedding_distance(ref_emb, emb);
                if (dist <= 0.6) {
                    db.set_face_person(face_id, person_id);
                    labeled++;
                    break;
                }
            }
        }
        sqlite3_finalize(stmt);

        std::cout << "Labeled " << labeled << " additional faces as \"" << name << "\"\n";
        return 0;

    } else if (subcmd == "rebuild") {
        // Parse match/filter flags
        std::vector<std::string> matches, filters;
        int parallel = -1;
        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--filter" && i + 1 < argc) {
                filters.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--parallel" && i + 1 < argc) {
                parallel = std::stoi(argv[++i]);
            } else if (arg == "--db") {
                i++; // already handled
            }
        }

        auto all_images = db.get_all_images();

        // Apply match/filter
        std::vector<ImageInfo> images;
        for (const auto& img : all_images) {
            if (passes_filters(img.path, matches, filters)) {
                images.push_back(img);
            }
        }

        if (images.empty()) {
            std::cout << "No matching images found.\n";
            return 0;
        }

        std::cout << "Rebuilding face data for " << images.size() << " images...\n";

        // Determine thread count
        int num_threads;
        if (parallel > 0) {
            num_threads = parallel;
        } else {
            num_threads = std::min(static_cast<int>(std::thread::hardware_concurrency()) / 2, 4);
            num_threads = std::max(num_threads, 1);
        }

        // Create per-thread detectors
        std::cout << "Loading face recognition models (" << num_threads << " instances)...\n";
        std::vector<std::unique_ptr<FaceDetector>> detectors;
        for (int i = 0; i < num_threads; i++) {
            detectors.push_back(std::make_unique<FaceDetector>());
        }

        std::atomic<size_t> next_img{0};
        std::atomic<size_t> total_faces{0};
        std::mutex db_mutex;

        auto worker = [&](int thread_id) {
            while (!g_interrupted) {
                size_t idx = next_img.fetch_add(1);
                if (idx >= images.size()) break;

                const auto& img_info = images[idx];
                cv::Mat img = decode_image(img_info.path);
                if (img.empty()) continue;

                auto faces = detectors[thread_id]->detect(img);

                {
                    std::lock_guard<std::mutex> lock(db_mutex);
                    int64_t image_id = db.get_image_id(img_info.path);
                    if (image_id >= 0) {
                        db.delete_faces_for_image(image_id);
                        db.begin_transaction();
                        for (const auto& face : faces) {
                            db.insert_face(image_id, face);
                            int64_t face_id = sqlite3_last_insert_rowid(db.db_ptr());
                            db.auto_label_face(face_id, face.embedding);
                        }
                        db.commit_transaction();
                        total_faces.fetch_add(faces.size());
                    }
                }

                fs::path p(img_info.path);
                std::string display = (p.parent_path().filename() / p.filename()).string();
                std::cout << "\r\033[K[" << (idx + 1) << "/" << images.size() << "] "
                          << display << std::flush;
            }
        };

        g_interrupted = 0;
        auto prev_handler = std::signal(SIGINT, sigint_handler);

        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; i++) {
            threads.emplace_back(worker, i);
        }
        for (auto& t : threads) t.join();

        std::signal(SIGINT, prev_handler);

        if (g_interrupted) {
            std::cout << "\n\nInterrupted. Progress saved.\n";
        }

        std::cout << "\n\nDone. " << total_faces.load() << " faces detected.\n";
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

    if (command == "--help" || command == "-h" || command == "help") {
        print_usage();
        return 0;
    }

    // Check for --help on any command
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
    }

    if (command == "scan") {
        if (argc < 3) {
            std::cerr << "Error: scan requires a directory argument\n";
            print_usage();
            return 1;
        }

        ScanOptions opts;
        opts.directory = argv[2];
        opts.db_path = default_db_path();

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--recursive") {
                opts.recursive = true;
            } else if (arg == "--force") {
                opts.force = true;
            } else if (arg == "--faces") {
                opts.faces = true;
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
            return cmd_scan(opts);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    } else if (command == "duplicates") {
        DuplicatesOptions opts;
        opts.db_path = default_db_path();

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
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(expand_tilde(argv[++i]));
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
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    } else if (command == "purge") {
        PurgeOptions opts;
        opts.db_path = default_db_path();

        for (int i = 2; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--db" && i + 1 < argc) {
                opts.db_path = argv[++i];
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                print_usage();
                return 1;
            }
        }

        try {
            return cmd_purge(opts);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
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

        for (int i = 3; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--format" && i + 1 < argc) {
                opts.format = argv[++i];
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--dry-run") {
                opts.dry_run = true;
            } else if (arg == "--on-conflict" && i + 1 < argc) {
                opts.on_conflict = argv[++i];
                if (opts.on_conflict != "skip" && opts.on_conflict != "overwrite" && opts.on_conflict != "rename") {
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
            return cmd_organize(opts);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
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
            return cmd_face(argc, argv, face_db);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    } else if (command == "models") {
        if (argc < 3 || std::string(argv[2]) != "download") {
            std::cerr << "Usage: phig models download\n";
            return 1;
        }
        try {
            if (download_face_models()) {
                std::cout << "All models downloaded successfully.\n";
                return 0;
            } else {
                std::cerr << "Model download failed.\n";
                return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    } else if (command == "search") {
        SearchOptions opts;
        opts.db_path = default_db_path();

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
            } else if (arg == "--match" && i + 1 < argc) {
                opts.matches.push_back(expand_tilde(argv[++i]));
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(expand_tilde(argv[++i]));
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
            std::cerr << "Error: " << e.what() << "\n";
            return 1;
        }
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }
}
