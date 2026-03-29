#include "scanner.h"
#include "hasher.h"
#include "exif.h"
#include "database.h"
#include "types.h"
#include "filters.h"

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

namespace fs = std::filesystem;

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
              << "\nScan flags:\n"
              << "  --recursive              Scan subdirectories\n"
              << "  --on-error warn|fail     Behavior for unreadable files (default: warn)\n"
              << "  --force                  Re-hash all files, ignore mtime check\n"
              << "  --db <path>              Database path\n"
              << "\nDuplicates flags:\n"
              << "  --type exact|near|all    Duplicate type to find (default: all)\n"
              << "  --threshold [0-64]       Hamming distance for near duplicates (default: 5)\n"
              << "  --format text|csv|json   Output format (default: text)\n"
              << "  --output <path>          Write to file instead of stdout\n"
              << "  --match <glob>           Show groups containing matching files (repeatable)\n"
              << "  --filter <glob>          Exclude files matching glob (repeatable, wins over --match)\n"
              << "  --path <directory>       Only check files under this directory\n"
              << "  --db <path>              Database path\n"
              << "\nCopy/Move flags (cp, mv):\n"
              << "  --format <string>        Output path format (default: %Y/%m/%original)\n"
              << "                           Tokens: %Y %m %d %camera %make %original\n"
              << "  --match <glob>           Only files matching glob (repeatable)\n"
              << "  --filter <glob>          Exclude files matching glob (repeatable, wins over --match)\n"
              << "  --path <directory>       Only files from this directory\n"
              << "  --dry-run                Show what would happen without doing it\n"
              << "  --on-conflict skip|overwrite|rename  (default: skip)\n"
              << "  --db <path>              Database path\n"
              << "\nPurge flags:\n"
              << "  --match <glob>           Remove entries matching filename glob (repeatable)\n"
              << "  --filter <glob>          Exclude files matching glob (repeatable, wins over --match)\n"
              << "  --path <directory>       Remove entries under this directory\n"
              << "  --dry-run                Show what would be removed\n"
              << "  --db <path>              Database path\n";
}

// ---- Scan command ----

struct ScanOptions {
    std::string directory;
    bool recursive = false;
    bool fail_on_error = false;
    bool force = false;
    std::string db_path;
};

ImageInfo process_file(const fs::path& filepath) {
    ImageInfo info;
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

    // Phash + dimensions
    auto phash_result = compute_phash(info.path);
    info.phash = phash_result.hash;
    info.width = phash_result.width;
    info.height = phash_result.height;

    // EXIF
    info.exif_json = extract_exif_json(info.path);

    return info;
}

int cmd_scan(const ScanOptions& opts) {
    auto start_time = std::chrono::steady_clock::now();

    Database db(opts.db_path);
    db.init_schema();

    std::cout << "Scanning: " << opts.directory
              << (opts.recursive ? " (recursive)" : "") << "\n";
    std::cout << "Database: " << opts.db_path << "\n";

    // Collect files
    auto files = scan_directory(opts.directory, opts.recursive);
    std::cout << "Found " << files.size() << " files\n";

    // Process files with thread pool
    std::atomic<size_t> processed{0};
    std::atomic<size_t> skipped{0};
    std::atomic<size_t> errors{0};

    if (!files.empty()) {
    const int num_threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<size_t> next_file{0};

    std::mutex db_mutex;
    std::vector<ImageInfo> batch;
    const size_t batch_size = 50;

    auto flush_batch = [&]() {
        if (!batch.empty()) {
            db.insert_batch(batch);
            batch.clear();
        }
    };

    auto worker = [&]() {
        while (true) {
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
                    skipped.fetch_add(1);
                    size_t done = processed.load() + skipped.load() + errors.load();
                    std::cout << "\r[" << done << "/" << files.size() << "] " << std::flush;
                    continue;
                }
            }

            try {
                auto info = process_file(filepath);

                std::lock_guard<std::mutex> lock(db_mutex);
                batch.push_back(std::move(info));
                if (batch.size() >= batch_size) {
                    flush_batch();
                }
                processed.fetch_add(1);
            } catch (const std::exception& e) {
                errors.fetch_add(1);
                if (opts.fail_on_error) {
                    std::cerr << "\nError processing " << filepath << ": " << e.what() << "\n";
                    throw;
                } else {
                    std::cerr << "\nWarning: skipping " << filepath << ": " << e.what() << "\n";
                }
            }

            size_t done = processed.load() + skipped.load() + errors.load();
            std::cout << "\r\033[K[" << done << "/" << files.size() << "] "
                      << filepath.filename().string() << std::flush;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    // Flush remaining
    flush_batch();
    } // end if (!files.empty())

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
              << "  Removed:   " << removed_count << "\n"
              << "  Total DB:  " << db.count() << " images\n";

    return 0;
}

// ---- Duplicates command ----

struct DuplicatesOptions {
    std::string type = "all";      // exact, near, all
    int threshold = 5;             // hamming distance for near duplicates
    std::string format = "text";   // text, csv, json
    std::string output;            // empty = stdout
    std::string path_prefix;       // empty = all, otherwise filter by path prefix
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
            if (passes_filters(img.filename, matches, filters)) {
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

    bool do_exact = (opts.type == "all" || opts.type == "exact");
    bool do_near = (opts.type == "all" || opts.type == "near");

    // Compare against full DB (within path prefix), then filter results
    std::vector<DuplicateGroup> exact_groups;
    std::set<std::string> exact_sha256s;

    if (do_exact) {
        auto groups = db.get_exact_duplicates(opts.path_prefix);
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
        auto all_images = db.get_all_images(opts.path_prefix);
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
    std::string path_prefix;        // filter by source directory
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

    auto images = db.get_all_images(opts.path_prefix);
    std::cout << "Found " << images.size() << " images in database";
    if (!opts.path_prefix.empty()) {
        std::cout << " under " << opts.path_prefix;
    }
    std::cout << "\n";

    // Apply match/filter
    bool has_filters = !opts.matches.empty() || !opts.filters.empty();
    if (has_filters) {
        images.erase(
            std::remove_if(images.begin(), images.end(),
                [&](const ImageInfo& img) {
                    return !passes_filters(img.filename, opts.matches, opts.filters);
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
    std::string path_prefix;    // filter by directory
    bool dry_run = false;
    std::string db_path;
};

int cmd_purge(const PurgeOptions& opts) {
    if (opts.matches.empty() && opts.path_prefix.empty()) {
        std::cerr << "Error: purge requires at least --match or --path\n";
        return 1;
    }

    Database db(opts.db_path);

    // Get all images, filtered by path prefix
    auto all_images = db.get_all_images(opts.path_prefix);

    // Apply match/filter
    std::vector<std::string> matched;
    for (const auto& img : all_images) {
        if (passes_filters(img.filename, opts.matches, opts.filters)) {
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

// ---- Main ----

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];

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
                opts.matches.push_back(argv[++i]);
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(argv[++i]);
            } else if (arg == "--path" && i + 1 < argc) {
                opts.path_prefix = fs::canonical(argv[++i]).string();
                if (opts.path_prefix.back() != '/') opts.path_prefix += '/';
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
                opts.matches.push_back(argv[++i]);
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(argv[++i]);
            } else if (arg == "--path" && i + 1 < argc) {
                opts.path_prefix = fs::canonical(argv[++i]).string();
                if (opts.path_prefix.back() != '/') opts.path_prefix += '/';
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
                opts.matches.push_back(argv[++i]);
            } else if (arg == "--filter" && i + 1 < argc) {
                opts.filters.push_back(argv[++i]);
            } else if (arg == "--path" && i + 1 < argc) {
                opts.path_prefix = fs::canonical(argv[++i]).string();
                if (opts.path_prefix.back() != '/') opts.path_prefix += '/';
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
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return 1;
    }
}
