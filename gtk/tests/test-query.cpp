#include <glib.h>

#include <string>
#include <vector>

#include "query.h"

namespace {

using phig::gtk::Chip;
using phig::gtk::ChipKind;
using phig::gtk::Filter;

// g_assert_cmpstr keeps only the pointer it is given, so a temporary string
// must outlive the macro: here it lives until this call returns.
void assert_text(const std::string& got, const char* want) {
    g_assert_cmpstr(got.c_str(), ==, want);
}

std::vector<std::string> labels(const Filter& filter) {
    std::vector<std::string> out;
    for (const Chip& chip : phig::gtk::chips(filter)) out.push_back(chip.label);
    return out;
}

Filter everything_set() {
    Filter f;
    f.tags = {"trip", "beach"};
    f.gallery = "Travel";
    f.favorite = true;
    f.after = "2024-01-01";
    f.before = "2025-01-01";
    f.camera = "EOS";
    f.make = "Canon";
    f.media = "video";
    f.extension = "mov";
    return f;
}

void test_chips_follow_the_filter_in_a_fixed_order() {
    const std::vector<std::string> want = {
        "Favorites",         "Gallery: Travel", "Tag: trip",   "Tag: beach", "From 2024-01-01",
        "Before 2025-01-01", "Camera: EOS",     "Make: Canon", "Videos",     "Extension: mov"};
    const std::vector<std::string> got = labels(everything_set());
    g_assert_cmpuint(got.size(), ==, want.size());
    for (size_t i = 0; i < want.size(); i++) g_assert_cmpstr(got[i].c_str(), ==, want[i].c_str());
    g_assert_true(phig::gtk::chips(Filter{}).empty());
}

void test_removing_a_chip_clears_only_its_field() {
    Filter f = everything_set();
    for (const Chip& chip : phig::gtk::chips(f))
        if (chip.kind == ChipKind::Tag && chip.value == "trip") phig::gtk::remove_chip(f, chip);
    g_assert_cmpuint(f.tags.size(), ==, 1);
    g_assert_cmpstr(f.tags[0].c_str(), ==, "beach");
    g_assert_cmpstr(f.gallery.c_str(), ==, "Travel");

    for (const Chip& chip : phig::gtk::chips(f)) phig::gtk::remove_chip(f, chip);
    g_assert_true(f == Filter{});
}

void test_a_scope_replaces_the_filter() {
    using phig::gtk::Scope;
    using phig::gtk::ScopeKind;
    g_assert_true(phig::gtk::filter_for(Scope{ScopeKind::All, {}}) == Filter{});
    g_assert_true(phig::gtk::filter_for(Scope{ScopeKind::Favorites, {}}).favorite);
    assert_text(phig::gtk::filter_for(Scope{ScopeKind::Gallery, "Home"}).gallery, "Home");
    const Filter tagged = phig::gtk::filter_for(Scope{ScopeKind::Tag, "trip"});
    g_assert_cmpuint(tagged.tags.size(), ==, 1);
    g_assert_cmpstr(tagged.tags[0].c_str(), ==, "trip");
    g_assert_false(tagged.favorite);
}

void test_the_form_applies_trimmed_fields_and_adds_a_tag_once() {
    Filter start;
    start.tags = {"trip"};
    phig::gtk::FilterForm form = phig::gtk::form_for(start);
    g_assert_true(form.tag.empty());
    form.tag = "  beach ";
    form.camera = " EOS ";
    form.extension = ".JPG";
    form.after = "2024-02-29";
    form.media = "image";

    auto applied = phig::gtk::apply_form(start, form);
    g_assert_true(applied.has_value());
    g_assert_cmpuint(applied->tags.size(), ==, 2);
    g_assert_cmpstr(applied->tags[1].c_str(), ==, "beach");
    g_assert_cmpstr(applied->camera.c_str(), ==, "EOS");
    g_assert_cmpstr(applied->extension.c_str(), ==, "JPG");
    g_assert_cmpstr(applied->after.c_str(), ==, "2024-02-29");
    g_assert_cmpstr(applied->media.c_str(), ==, "image");

    form.tag = "beach";
    auto again = phig::gtk::apply_form(*applied, form);
    g_assert_cmpuint(again->tags.size(), ==, 2);

    // Emptying a field clears it.
    phig::gtk::FilterForm cleared = phig::gtk::form_for(*again);
    cleared.camera = "";
    g_assert_true(phig::gtk::apply_form(*again, cleared)->camera.empty());
}

void test_the_form_refuses_a_bad_date_or_media() {
    phig::gtk::FilterForm form;
    form.after = "2024/01/01";
    auto refused = phig::gtk::apply_form(Filter{}, form);
    g_assert_false(refused.has_value());
    g_assert_nonnull(g_strstr_len(refused.error().c_str(), -1, "From"));

    form.after = "";
    form.before = "2024-13-01";
    g_assert_false(phig::gtk::apply_form(Filter{}, form).has_value());

    form.before = "";
    form.media = "audio";
    g_assert_false(phig::gtk::apply_form(Filter{}, form).has_value());
}

void test_a_date_is_yyyy_mm_dd() {
    g_assert_true(phig::gtk::is_date("2026-11-13"));
    g_assert_true(phig::gtk::is_date("1999-01-31"));
    g_assert_false(phig::gtk::is_date("2026-11-1"));
    g_assert_false(phig::gtk::is_date("2026:11:13"));
    g_assert_false(phig::gtk::is_date("2026-00-13"));
    g_assert_false(phig::gtk::is_date("2026-11-32"));
    g_assert_false(phig::gtk::is_date("20a6-11-13"));
    g_assert_false(phig::gtk::is_date(""));
}

void test_what_narrows_and_what_annotations_select() {
    phig::gtk::Query query;
    g_assert_false(phig::gtk::narrows(query));
    query.sort = phig::gtk::Sort::Size;
    g_assert_false(phig::gtk::narrows(query));
    query.search = "beach";
    g_assert_true(phig::gtk::narrows(query));
    query.search.clear();
    query.filter.camera = "EOS";
    g_assert_true(phig::gtk::narrows(query));

    g_assert_false(phig::gtk::selects_by_annotation(query.filter));
    query.filter.favorite = true;
    g_assert_true(phig::gtk::selects_by_annotation(query.filter));
    g_assert_true(
        phig::gtk::selects_by_annotation(phig::gtk::filter_for({phig::gtk::ScopeKind::Tag, "x"})));
    g_assert_true(phig::gtk::selects_by_annotation(
        phig::gtk::filter_for({phig::gtk::ScopeKind::Gallery, "x"})));
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/query/chips-follow-the-filter-in-a-fixed-order",
                    test_chips_follow_the_filter_in_a_fixed_order);
    g_test_add_func("/query/removing-a-chip-clears-only-its-field",
                    test_removing_a_chip_clears_only_its_field);
    g_test_add_func("/query/a-scope-replaces-the-filter", test_a_scope_replaces_the_filter);
    g_test_add_func("/query/the-form-applies-trimmed-fields-and-adds-a-tag-once",
                    test_the_form_applies_trimmed_fields_and_adds_a_tag_once);
    g_test_add_func("/query/the-form-refuses-a-bad-date-or-media",
                    test_the_form_refuses_a_bad_date_or_media);
    g_test_add_func("/query/a-date-is-yyyy-mm-dd", test_a_date_is_yyyy_mm_dd);
    g_test_add_func("/query/what-narrows-and-what-annotations-select",
                    test_what_narrows_and_what_annotations_select);
    return g_test_run();
}
