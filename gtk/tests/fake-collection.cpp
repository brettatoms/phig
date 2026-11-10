#include "fake-collection.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace phig::gtk {

struct Collection {
    uint32_t n_rows = 0;
    std::filesystem::path thumb_dir;
    std::atomic<bool> fail_pages{false};
    std::atomic<uint32_t> generate_count{0};
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool hold = false;  // guarded by hold_mutex
};

void CollectionClose::operator()(Collection* coll) const noexcept {
    std::error_code ec;
    std::filesystem::remove_all(coll->thumb_dir, ec);
    delete coll;
}

namespace {

constexpr int kThumbEdge = 64;

bool is_hex64(std::string_view s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return g_ascii_isxdigit(c) != FALSE; });
}

std::filesystem::path thumb_path(const Collection& coll, std::string_view sha256, int size) {
    return coll.thumb_dir / std::to_string(size) / (std::string(sha256) + ".png");
}

// Writes a solid width x height PNG to path, atomically.
bool write_png(const std::filesystem::path& path, int width, int height) {
    std::string pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, '\x80');
    GBytes* bytes = g_bytes_new(pixels.data(), pixels.size());
    GdkTexture* texture = gdk_memory_texture_new(width, height, GDK_MEMORY_R8G8B8A8, bytes,
                                                 static_cast<gsize>(width) * 4);
    GBytes* png = gdk_texture_save_to_png_bytes(texture);
    gsize size = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(png, &size));
    gboolean ok = g_file_set_contents(path.c_str(), data, static_cast<gssize>(size), nullptr);
    g_bytes_unref(png);
    g_object_unref(texture);
    g_bytes_unref(bytes);
    return ok;
}

}  // namespace

std::expected<uint32_t, std::string> count(Collection& coll) noexcept { return coll.n_rows; }

std::expected<std::vector<Row>, std::string> page(Collection& coll, uint32_t offset,
                                                  uint32_t limit) noexcept {
    if (coll.fail_pages.load()) return std::unexpected(std::string(fake::kPageError));
    const uint32_t end = offset >= coll.n_rows ? offset : std::min(coll.n_rows, offset + limit);
    std::vector<Row> rows;
    rows.reserve(end - offset);
    for (uint32_t i = offset; i < end; i++)
        rows.push_back(Row{static_cast<int64_t>(i) + 1, "/fake/" + std::to_string(i) + ".jpg",
                           fake::sha(i), std::to_string(i) + ".jpg"});
    return rows;
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

namespace fake {

CollectionPtr make(uint32_t n_rows) {
    CollectionPtr coll(new Collection());
    coll->n_rows = n_rows;
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

}  // namespace fake

}  // namespace phig::gtk
