#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "collection.h"

// The gallery's filter as the window presents it: removable chips, the
// sidebar's scopes and the filter popover's form. Plain functions over
// phig::gtk::Filter, so the window holds no filter logic of its own.
namespace phig::gtk {

enum class ChipKind { Favorite, Gallery, Tag, After, Before, Camera, Make, Media, Extension };

struct Chip {
    ChipKind kind;
    std::string value;  // the tag a Tag chip names; unused by the others
    std::string label;
};

// One chip per field that is set, in a fixed order: favorite, gallery, each
// tag, the dates, camera, make, media, extension.
std::vector<Chip> chips(const Filter& filter);

// Clears the field chip stands for.
void remove_chip(Filter& filter, const Chip& chip);

// A sidebar row. Choosing one replaces the filter with the scope's.
enum class ScopeKind { All, Favorites, Gallery, Tag };
struct Scope {
    ScopeKind kind = ScopeKind::All;
    std::string name;  // the gallery or tag
};
Filter filter_for(const Scope& scope);

// The filter popover's fields, as text.
struct FilterForm {
    std::string tag;  // added to the filter's tags
    std::string gallery;
    bool favorite = false;
    std::string after;
    std::string before;
    std::string camera;
    std::string make;
    std::string media;  // "image", "video" or ""
    std::string extension;
};

// The form showing filter: every field but tag, which adds rather than shows.
FilterForm form_for(const Filter& filter);

// filter with the form's fields applied, surrounding whitespace trimmed and a
// leading dot dropped from the extension. Fails, naming the field, on a date
// that is not YYYY-MM-DD or a media type other than image or video.
std::expected<Filter, std::string> apply_form(const Filter& filter, const FilterForm& form);

// Whether text is a YYYY-MM-DD date with a month from 01 to 12 and a day from
// 01 to 31.
bool is_date(std::string_view text);

// Whether the query narrows the collection at all. A query that does not and
// still selects nothing means the collection is empty.
bool narrows(const Query& query);

// Whether a tag, gallery or favorite write can change what the filter
// selects.
bool selects_by_annotation(const Filter& filter);

}  // namespace phig::gtk
