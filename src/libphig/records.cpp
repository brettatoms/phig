#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "internal.h"

namespace {
// Backing storage for one returned list. `strings` is a deque so that pushing
// more entries never reallocates existing ones — the phig_image pointers
// reference into these strings directly.
struct ListStorage {
    phig_image_list header;
    std::vector<phig_image> items;
    std::deque<std::string> strings;
};

const char* intern(ListStorage& st, const std::string& s) {
    st.strings.push_back(s);
    return st.strings.back().c_str();
}

phig_media_type media_from(const std::string& s) {
    if (s == "video") return PHIG_MEDIA_VIDEO;
    return PHIG_MEDIA_IMAGE;
}
}  // namespace

phig_image_list* phig_make_image_list(const std::vector<ImageInfo>& rows) {
    auto* st = new ListStorage();
    st->items.reserve(rows.size());
    for (const auto& r : rows) {
        phig_image im{};
        im.id = r.id;
        im.path = intern(*st, r.path);
        im.filename = intern(*st, r.filename);
        im.extension = intern(*st, r.extension);
        im.sha256 = intern(*st, r.sha256);
        im.file_size = r.file_size;
        im.created_at = intern(*st, r.created_at);
        im.modified_at = intern(*st, r.modified_at);
        im.effective_date = intern(*st, r.effective_date);
        im.width = r.width;
        im.height = r.height;
        im.media_type = media_from(r.media_type);
        im.duration_ms = r.duration_ms;
        im.gallery = intern(*st, r.gallery);
        im.blur_score = r.blur_score;
        im.favorite = r.favorite;
        st->items.push_back(im);
    }
    st->header.items = st->items.data();
    st->header.count = static_cast<int64_t>(st->items.size());
    // The header is the first member, so the list pointer round-trips to
    // ListStorage* in phig_image_list_free.
    return &st->header;
}

extern "C" void phig_image_list_free(phig_image_list* list) {
    if (list == nullptr) return;
    delete reinterpret_cast<ListStorage*>(list);
}

extern "C" void phig_string_free(char* s) { delete[] s; }
