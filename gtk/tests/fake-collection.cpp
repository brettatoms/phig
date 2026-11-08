#include "fake-collection.h"

#include <gtk/gtk.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <string>

struct PhigCollection {
    guint n_rows = 0;
    std::filesystem::path thumb_dir;
    std::atomic<bool> fail_pages{false};
    std::atomic<guint> generate_count{0};
};

namespace {

constexpr int kThumbEdge = 64;

std::string sha_of(guint index) {
    char sha[65];
    fake_collection_sha(index, sha);
    return sha;
}

bool is_hex64(const char* s) {
    if (s == nullptr) return false;
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        if (n >= 64 || !g_ascii_isxdigit(s[n])) return false;
    }
    return n == 64;
}

std::filesystem::path thumb_path(const PhigCollection& coll, const char* sha256, int size) {
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

extern "C" {

PhigCollection* fake_collection_new(guint n_rows) {
    auto* coll = new PhigCollection();
    coll->n_rows = n_rows;
    char* dir = g_dir_make_tmp("phig-gtk-fake-XXXXXX", nullptr);
    g_assert_nonnull(dir);
    coll->thumb_dir = dir;
    g_free(dir);
    return coll;
}

void fake_collection_sha(guint index, char out[65]) { g_snprintf(out, 65, "%064x", index); }

void fake_collection_fail_pages(PhigCollection* coll, gboolean fail) {
    coll->fail_pages = fail != FALSE;
}

guint fake_collection_generate_count(PhigCollection* coll) { return coll->generate_count.load(); }

void phig_collection_close(PhigCollection* coll) {
    if (coll == nullptr) return;
    std::error_code ec;
    std::filesystem::remove_all(coll->thumb_dir, ec);
    delete coll;
}

gboolean phig_collection_count(PhigCollection* coll, guint* out) {
    *out = coll->n_rows;
    return TRUE;
}

PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit) {
    if (coll->fail_pages.load()) return nullptr;
    guint end = offset >= coll->n_rows ? offset : std::min(coll->n_rows, offset + limit);
    PhigRowPage* page = phig_row_page_new(end - offset);
    for (guint i = offset; i < end; i++) {
        PhigRowData* row = &page->rows[i - offset];
        row->id = static_cast<int64_t>(i) + 1;
        row->path = g_strdup_printf("/fake/%u.jpg", i);
        row->filename = g_strdup_printf("%u.jpg", i);
        row->sha256 = g_strdup(sha_of(i).c_str());
    }
    return page;
}

PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path) {
    *path = nullptr;
    if (!is_hex64(sha256) || size <= 0) return PHIG_THUMB_ERROR;
    std::filesystem::path p = thumb_path(*coll, sha256, size);
    *path = g_strdup(p.c_str());
    std::error_code ec;
    return std::filesystem::exists(p, ec) ? PHIG_THUMB_PRESENT : PHIG_THUMB_ABSENT;
}

gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size) {
    if (image_id < 1 || image_id > static_cast<int64_t>(coll->n_rows) || size <= 0) return FALSE;
    std::string sha = sha_of(static_cast<guint>(image_id - 1));
    std::filesystem::path p = thumb_path(*coll, sha.c_str(), size);
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return TRUE;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (!write_png(p, kThumbEdge, kThumbEdge)) return FALSE;
    coll->generate_count++;
    return TRUE;
}

}  // extern "C"
