#include "fake-collection.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

namespace phig::gtk {

namespace {

// One row's metadata as the fake's writes leave it.
struct FakeRow {
    std::set<std::string> tags;
    std::string gallery;
    bool favorite = false;
};

}  // namespace

struct Collection {
    uint32_t n_rows = 0;
    std::filesystem::path thumb_dir;
    std::atomic<bool> fail_pages{false};
    std::atomic<uint32_t> generate_count{0};
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool hold = false;  // guarded by hold_mutex
    std::mutex page_hold_mutex;
    std::condition_variable page_hold_cv;
    bool hold_pages = false;  // guarded by page_hold_mutex
    std::atomic<uint32_t> pages_in_progress{0};
    std::mutex rows_mutex;
    std::vector<FakeRow> meta;  // by row index; guarded by rows_mutex
};

void CollectionClose::operator()(Collection* coll) const noexcept {
    std::error_code ec;
    std::filesystem::remove_all(coll->thumb_dir, ec);
    delete coll;
}

struct IndexRun {
    IndexOptions options;
    size_t done = 0;
    bool cancelled = false;
};

void IndexRunClose::operator()(IndexRun* run) const noexcept { delete run; }

namespace {

constexpr int kThumbEdge = 64;

bool is_hex64(std::string_view s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return g_ascii_isxdigit(c) != FALSE; });
}

std::filesystem::path thumb_path(const Collection& coll, std::string_view sha256, int size) {
    return coll.thumb_dir / std::to_string(size) / (std::string(sha256) + ".png");
}

// A solid width x height texture.
GRef<GdkTexture> solid_texture(int width, int height) {
    std::string pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, '\x80');
    GBytes* bytes = g_bytes_new(pixels.data(), pixels.size());
    GRef<GdkTexture> texture = adopt(gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8,
                                                            bytes, static_cast<gsize>(width) * 4));
    g_bytes_unref(bytes);
    return texture;
}

// Writes a solid width x height PNG to path, atomically.
bool write_png(const std::filesystem::path& path, int width, int height) {
    GRef<GdkTexture> texture = solid_texture(width, height);
    GBytes* png = gdk_texture_save_to_png_bytes(texture.get());
    gsize size = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(png, &size));
    gboolean ok = g_file_set_contents(path.c_str(), data, static_cast<gssize>(size), nullptr);
    g_bytes_unref(png);
    return ok;
}

bool known(const Collection& coll, int64_t id) {
    return id >= 1 && id <= static_cast<int64_t>(coll.n_rows);
}

Row row_for(uint32_t index) {
    return Row{static_cast<int64_t>(index) + 1, "/fake/" + std::to_string(index) + ".jpg",
               fake::sha(index), std::to_string(index) + ".jpg"};
}

// Applies change to every known id's row.
template <typename Change>
WriteResult fake_write(Collection& coll, const std::vector<int64_t>& ids, const Change& change) {
    std::lock_guard lock(coll.rows_mutex);
    for (int64_t id : ids)
        if (known(coll, id)) change(coll.meta[static_cast<size_t>(id - 1)]);
    return {};
}

std::mutex index_mutex;
std::string index_last_catalog;   // guarded by index_mutex
IndexOptions index_last_options;  // guarded by index_mutex

}  // namespace

std::expected<std::vector<int64_t>, std::string> ids(Collection& coll,
                                                     const Query& query) noexcept {
    std::lock_guard lock(coll.rows_mutex);
    const Filter& f = query.filter;
    std::vector<int64_t> out;
    for (uint32_t i = 0; i < coll.n_rows; i++) {
        const FakeRow& m = coll.meta[i];
        if (f.favorite && !m.favorite) continue;
        if (!f.gallery.empty() && m.gallery != f.gallery) continue;
        if (!std::all_of(f.tags.begin(), f.tags.end(),
                         [&m](const std::string& t) { return m.tags.contains(t); }))
            continue;
        if (!query.search.empty() &&
            (std::to_string(i) + ".jpg").find(query.search) == std::string::npos)
            continue;
        out.push_back(static_cast<int64_t>(i) + 1);
    }
    if (query.sort == Sort::DateOldest) std::reverse(out.begin(), out.end());
    return out;
}

std::expected<std::vector<Row>, std::string> rows(Collection& coll,
                                                  std::span<const int64_t> ids) noexcept {
    coll.pages_in_progress++;
    {
        std::unique_lock lock(coll.page_hold_mutex);
        coll.page_hold_cv.wait(lock, [&coll] { return !coll.hold_pages; });
    }
    struct Leave {
        Collection& coll;
        ~Leave() { coll.pages_in_progress--; }
    } leave{coll};
    if (coll.fail_pages.load()) return std::unexpected(std::string(fake::kPageError));
    std::vector<Row> out;
    for (int64_t id : ids)
        if (known(coll, id)) out.push_back(row_for(static_cast<uint32_t>(id - 1)));
    return out;
}

std::expected<Counts, std::string> counts(Collection& coll) noexcept {
    std::lock_guard lock(coll.rows_mutex);
    Counts out;
    out.all = coll.n_rows;
    std::map<std::string, uint32_t> galleries;
    std::map<std::string, uint32_t> tags;
    for (const FakeRow& m : coll.meta) {
        if (m.favorite) out.favorites++;
        if (!m.gallery.empty()) galleries[m.gallery]++;
        for (const std::string& t : m.tags) tags[t]++;
    }
    for (const auto& [name, n] : galleries) out.galleries.push_back(NamedCount{name, n});
    for (const auto& [name, n] : tags) out.tags.push_back(NamedCount{name, n});
    return out;
}

std::expected<Details, std::string> details(Collection& coll, int64_t id) noexcept {
    std::lock_guard lock(coll.rows_mutex);
    if (!known(coll, id)) return std::unexpected("fake: no row " + std::to_string(id));
    const auto index = static_cast<uint32_t>(id - 1);
    const Row row = row_for(index);
    const FakeRow& m = coll.meta[index];
    Details d;
    d.id = id;
    d.path = row.path;
    d.filename = row.filename;
    d.sha256 = row.sha256;
    d.media_type = "image";
    d.date = "2026-01-01 00:00:00";
    d.tags.assign(m.tags.begin(), m.tags.end());
    d.gallery = m.gallery;
    d.favorite = m.favorite;
    return d;
}

std::expected<GRef<GdkTexture>, std::string> image(Collection& coll, int64_t id,
                                                   int max_edge) noexcept {
    if (!known(coll, id)) return std::unexpected("fake: no row " + std::to_string(id));
    if (max_edge <= 0) return std::unexpected(std::string("fake: bad size"));
    const int edge = std::min(kThumbEdge, max_edge);
    return solid_texture(edge, edge);
}

WriteResult tag_add(Collection& coll, const std::vector<int64_t>& ids,
                    const std::string& tag) noexcept {
    if (tag.empty()) return std::unexpected(WriteError{false, "fake: empty tag"});
    return fake_write(coll, ids, [&tag](FakeRow& m) { m.tags.insert(tag); });
}

WriteResult tag_remove(Collection& coll, const std::vector<int64_t>& ids,
                       const std::string& tag) noexcept {
    if (tag.empty()) return std::unexpected(WriteError{false, "fake: empty tag"});
    return fake_write(coll, ids, [&tag](FakeRow& m) { m.tags.erase(tag); });
}

WriteResult gallery_set(Collection& coll, const std::vector<int64_t>& ids,
                        const std::string& gallery) noexcept {
    if (gallery.empty()) return std::unexpected(WriteError{false, "fake: empty gallery"});
    return fake_write(coll, ids, [&gallery](FakeRow& m) { m.gallery = gallery; });
}

WriteResult gallery_clear(Collection& coll, const std::vector<int64_t>& ids) noexcept {
    return fake_write(coll, ids, [](FakeRow& m) { m.gallery.clear(); });
}

WriteResult favorite_set(Collection& coll, const std::vector<int64_t>& ids,
                         bool favorite) noexcept {
    return fake_write(coll, ids, [favorite](FakeRow& m) { m.favorite = favorite; });
}

std::expected<IndexRunPtr, std::string> index_start(const std::string& catalog_dir,
                                                    IndexOptions options) noexcept {
    if (options.directory.empty()) return std::unexpected(std::string("fake: no directory"));
    {
        std::lock_guard lock(index_mutex);
        index_last_catalog = catalog_dir;
        index_last_options = options;
    }
    IndexRunPtr run(new IndexRun());
    run->options = std::move(options);
    return run;
}

std::optional<IndexProgress> index_poll(IndexRun& run) noexcept {
    if (run.cancelled || run.done == fake::kIndexFiles ||
        run.options.directory == fake::kIndexFailDirectory)
        return std::nullopt;
    run.done++;
    return IndexProgress{run.done, fake::kIndexFiles,
                         run.done, 0,
                         0,        run.options.directory + "/" + std::to_string(run.done) + ".jpg"};
}

void index_cancel(IndexRun& run) noexcept { run.cancelled = true; }

std::optional<std::expected<IndexSummary, std::string>> index_result(IndexRun& run) noexcept {
    if (run.options.directory == fake::kIndexFailDirectory)
        return std::unexpected(std::string(fake::kIndexError));
    if (run.cancelled) return IndexSummary{fake::kIndexFiles, run.done, 0, 0, false};
    if (run.done < fake::kIndexFiles) return std::nullopt;
    return IndexSummary{fake::kIndexFiles, fake::kIndexFiles, 0, 0, true};
}

ThumbLocation thumb_lookup(Collection& coll, std::string_view sha256, int size) noexcept {
    if (!is_hex64(sha256) || size <= 0) return {};
    std::filesystem::path p = thumb_path(coll, sha256, size);
    std::error_code ec;
    const ThumbLookup state =
        std::filesystem::exists(p, ec) ? ThumbLookup::Present : ThumbLookup::Absent;
    return {state, p.string()};
}

ThumbGenerate thumb_generate(Collection& coll, int64_t image_id, int size) noexcept {
    if (size <= 0) return ThumbGenerate::Failed;
    if (image_id < 1 || image_id > static_cast<int64_t>(coll.n_rows))
        return ThumbGenerate::NoSuchRow;
    {
        std::unique_lock lock(coll.hold_mutex);
        coll.hold_cv.wait(lock, [&coll] { return !coll.hold; });
    }
    std::filesystem::path p =
        thumb_path(coll, fake::sha(static_cast<uint32_t>(image_id - 1)), size);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return ThumbGenerate::Ok;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!write_png(p, kThumbEdge, kThumbEdge)) return ThumbGenerate::Failed;
    coll.generate_count++;
    return ThumbGenerate::Ok;
}

namespace {

std::mutex open_mutex;
std::condition_variable open_cv;
bool open_held = false;  // guarded by open_mutex
std::string open_last;   // guarded by open_mutex

}  // namespace

std::expected<CollectionPtr, OpenError> open(const std::string& catalog_dir) noexcept {
    std::unique_lock lock(open_mutex);
    open_cv.wait(lock, [] { return !open_held; });
    // Read only after the hold, so a caller that passed a reference to storage
    // it then freed is caught under ASan.
    open_last = catalog_dir;
    if (open_last == fake::kMissingCatalog)
        return std::unexpected(OpenError{true, "fake: no catalog at " + open_last});
    lock.unlock();
    return fake::make(3);
}

std::expected<std::string, std::string> default_catalog() noexcept {
    return std::string("/fake/default");
}

namespace fake {

CollectionPtr make(uint32_t n_rows) {
    CollectionPtr coll(new Collection());
    coll->n_rows = n_rows;
    coll->meta.resize(n_rows);
    char* dir = g_dir_make_tmp("phig-gtk-fake-XXXXXX", nullptr);
    g_assert_nonnull(dir);
    coll->thumb_dir = dir;
    g_free(dir);
    return coll;
}

std::string sha(uint32_t index) {
    char out[65];
    g_snprintf(out, sizeof out, "%064x", index);
    return out;
}

void fail_pages(Collection& coll, bool fail) { coll.fail_pages = fail; }

uint32_t generate_count(Collection& coll) { return coll.generate_count.load(); }

void hold_generation(Collection& coll, bool hold) {
    {
        std::lock_guard lock(coll.hold_mutex);
        coll.hold = hold;
    }
    coll.hold_cv.notify_all();
}

void hold_open(bool hold) {
    {
        std::lock_guard lock(open_mutex);
        open_held = hold;
    }
    open_cv.notify_all();
}

std::string last_opened() {
    std::lock_guard lock(open_mutex);
    return open_last;
}

void hold_pages(Collection& coll, bool hold) {
    {
        std::lock_guard lock(coll.page_hold_mutex);
        coll.hold_pages = hold;
    }
    coll.page_hold_cv.notify_all();
}

uint32_t pages_in_progress(Collection& coll) { return coll.pages_in_progress.load(); }

std::string last_index_catalog() {
    std::lock_guard lock(index_mutex);
    return index_last_catalog;
}

IndexOptions last_index_options() {
    std::lock_guard lock(index_mutex);
    return index_last_options;
}

}  // namespace fake

}  // namespace phig::gtk
