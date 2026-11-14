#include "query.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace phig::gtk {

namespace {

std::string trimmed(std::string_view text) {
    auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return std::string(text);
}

int two_digits(std::string_view text, size_t at) {
    return (text[at] - '0') * 10 + text[at + 1] - '0';
}

}  // namespace

std::vector<Chip> chips(const Filter& filter) {
    std::vector<Chip> out;
    if (filter.favorite) out.push_back({ChipKind::Favorite, {}, "Favorites"});
    if (!filter.gallery.empty())
        out.push_back({ChipKind::Gallery, {}, "Gallery: " + filter.gallery});
    for (const std::string& tag : filter.tags) out.push_back({ChipKind::Tag, tag, "Tag: " + tag});
    if (!filter.after.empty()) out.push_back({ChipKind::After, {}, "From " + filter.after});
    if (!filter.before.empty()) out.push_back({ChipKind::Before, {}, "Before " + filter.before});
    if (!filter.camera.empty()) out.push_back({ChipKind::Camera, {}, "Camera: " + filter.camera});
    if (!filter.make.empty()) out.push_back({ChipKind::Make, {}, "Make: " + filter.make});
    if (filter.media == "image") out.push_back({ChipKind::Media, {}, "Images"});
    if (filter.media == "video") out.push_back({ChipKind::Media, {}, "Videos"});
    if (!filter.extension.empty())
        out.push_back({ChipKind::Extension, {}, "Extension: " + filter.extension});
    return out;
}

void remove_chip(Filter& filter, const Chip& chip) {
    switch (chip.kind) {
        case ChipKind::Favorite:
            filter.favorite = false;
            break;
        case ChipKind::Gallery:
            filter.gallery.clear();
            break;
        case ChipKind::Tag:
            std::erase(filter.tags, chip.value);
            break;
        case ChipKind::After:
            filter.after.clear();
            break;
        case ChipKind::Before:
            filter.before.clear();
            break;
        case ChipKind::Camera:
            filter.camera.clear();
            break;
        case ChipKind::Make:
            filter.make.clear();
            break;
        case ChipKind::Media:
            filter.media.clear();
            break;
        case ChipKind::Extension:
            filter.extension.clear();
            break;
    }
}

Filter filter_for(const Scope& scope) {
    Filter filter;
    switch (scope.kind) {
        case ScopeKind::All:
            break;
        case ScopeKind::Favorites:
            filter.favorite = true;
            break;
        case ScopeKind::Gallery:
            filter.gallery = scope.name;
            break;
        case ScopeKind::Tag:
            filter.tags = {scope.name};
            break;
    }
    return filter;
}

FilterForm form_for(const Filter& filter) {
    FilterForm form;
    form.gallery = filter.gallery;
    form.favorite = filter.favorite;
    form.after = filter.after;
    form.before = filter.before;
    form.camera = filter.camera;
    form.make = filter.make;
    form.media = filter.media;
    form.extension = filter.extension;
    return form;
}

std::expected<Filter, std::string> apply_form(const Filter& filter, const FilterForm& form) {
    Filter out = filter;
    out.after = trimmed(form.after);
    out.before = trimmed(form.before);
    if (!out.after.empty() && !is_date(out.after))
        return std::unexpected("From must be a date written YYYY-MM-DD.");
    if (!out.before.empty() && !is_date(out.before))
        return std::unexpected("Before must be a date written YYYY-MM-DD.");
    if (!form.media.empty() && form.media != "image" && form.media != "video")
        return std::unexpected("Media must be images or videos.");
    out.media = form.media;
    out.gallery = trimmed(form.gallery);
    out.favorite = form.favorite;
    out.camera = trimmed(form.camera);
    out.make = trimmed(form.make);
    out.extension = trimmed(form.extension);
    if (out.extension.starts_with('.')) out.extension.erase(0, 1);
    const std::string tag = trimmed(form.tag);
    if (!tag.empty() && std::find(out.tags.begin(), out.tags.end(), tag) == out.tags.end())
        out.tags.push_back(tag);
    return out;
}

bool is_date(std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) return false;
    const int month = two_digits(text, 5);
    const int day = two_digits(text, 8);
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

bool narrows(const Query& query) { return !(query.filter == Filter{}) || !query.search.empty(); }

bool selects_by_annotation(const Filter& filter) {
    return filter.favorite || !filter.gallery.empty() || !filter.tags.empty();
}

}  // namespace phig::gtk
