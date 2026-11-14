#include <expected>
#include <utility>
#include <vector>

#include "app.h"
#include "background.h"
#include "connect.h"
#include "format.h"
#include "gallery-selection.h"
#include "query.h"

namespace {

using phig::gtk::connect;

// The Sort each entry of the sort dropdown stands for, in order.
constexpr phig::gtk::Sort kSorts[] = {phig::gtk::Sort::DateNewest, phig::gtk::Sort::DateOldest,
                                      phig::gtk::Sort::Filename, phig::gtk::Sort::Size};
constexpr const char* kSortNames[] = {"Newest First", "Oldest First", "Filename", "Largest First",
                                      nullptr};

// The media value each entry of the filter's media dropdown stands for.
constexpr const char* kMedia[] = {"", "image", "video"};
constexpr const char* kMediaNames[] = {"Images and Videos", "Images", "Videos", nullptr};

void update_chips(PhigApp& app);

// The query changed: show its chips and reload the gallery for it.
void query_changed(PhigApp& app) {
    update_chips(app);
    reload(app);
}

void on_page_fetch_failed(PhigApp& app) {
    // One toast for the whole session, not one per failed page: a scroll that
    // outruns several page fetches would otherwise stack a toast per failure.
    // The affected cells keep their placeholders.
    if (app.window == nullptr || app.page_fetch_toast_shown) return;
    app.page_fetch_toast_shown = true;
    show_toast(app, "Some images could not be loaded");
}

void attach_model(PhigApp& app, std::vector<int64_t> ids) {
    const bool empty = ids.empty();
    GRef<PhigGalleryModel> model = adopt(phig_gallery_model_new(*app.coll, std::move(ids)));
    PhigApp* a = &app;
    connect<void(PhigGalleryModel*, const char*)>(
        model.get(), "page-fetch-failed",
        [a](PhigGalleryModel*, const char*) noexcept { on_page_fetch_failed(*a); });
    PhigGallerySelection* selection = phig_gallery_selection_new(model.get());
    connect<void(GtkSelectionModel*, guint, guint)>(
        selection, "selection-changed",
        [a](GtkSelectionModel*, guint, guint) noexcept { update_selection_bar(*a); });
    app.selection = GTK_SELECTION_MODEL(selection);
    gtk_grid_view_set_model(app.grid, app.selection);
    g_object_unref(selection);
    // The previous model's last reference drops here, and its dispose waits for
    // a page fetch it has in flight.
    app.model = std::move(model);
    update_selection_bar(app);

    if (empty)
        show_status(app, phig::gtk::narrows(app.query) ? Status::NoMatches : Status::NoPhotos);
    else
        gtk_stack_set_visible_child_name(app.content, "grid");
}

// A sidebar row's label and count.
GtkWidget* sidebar_row(const std::string& name, uint32_t count) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    GtkWidget* label = gtk_label_new(name.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    GtkWidget* number = gtk_label_new(std::to_string(count).c_str());
    gtk_widget_add_css_class(number, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), number);
    return box;
}

void add_scope_row(PhigApp& app, const std::string& name, uint32_t count, phig::gtk::Scope scope) {
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), sidebar_row(name, count));
    g_object_set_data_full(G_OBJECT(row), "phig-scope", new phig::gtk::Scope(std::move(scope)),
                           [](gpointer p) noexcept { delete static_cast<phig::gtk::Scope*>(p); });
    gtk_list_box_append(app.sidebar, row);
}

void add_heading_row(PhigApp& app, const char* text) {
    GtkWidget* label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_margin_start(label, 6);
    gtk_widget_set_margin_top(label, 12);
    gtk_widget_set_margin_bottom(label, 3);
    GtkWidget* row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_append(app.sidebar, row);
}

void fill_sidebar(PhigApp& app, const phig::gtk::Counts& counts) {
    using phig::gtk::Scope;
    using phig::gtk::ScopeKind;
    gtk_list_box_remove_all(app.sidebar);
    add_scope_row(app, "All Photos", counts.all, Scope{ScopeKind::All, {}});
    add_scope_row(app, "Favorites", counts.favorites, Scope{ScopeKind::Favorites, {}});
    if (!counts.galleries.empty()) add_heading_row(app, "Galleries");
    for (const phig::gtk::NamedCount& g : counts.galleries)
        add_scope_row(app, g.name, g.count, Scope{ScopeKind::Gallery, g.name});
    if (!counts.tags.empty()) add_heading_row(app, "Tags");
    for (const phig::gtk::NamedCount& t : counts.tags)
        add_scope_row(app, t.name, t.count, Scope{ScopeKind::Tag, t.name});
}

void refresh_counts(PhigApp& app) {
    if (!app.coll) return;
    phig::gtk::Collection* coll = app.coll.get();
    PhigApp* a = &app;
    phig::gtk::run_in_background<std::expected<phig::gtk::Counts, std::string>>(
        [coll]() noexcept { return phig::gtk::counts(*coll); },
        [a](std::expected<phig::gtk::Counts, std::string> counts) noexcept {
            if (a->window == nullptr) return;
            if (!counts) {
                show_toast(*a, "Could not count the collection: " + counts.error());
                return;
            }
            fill_sidebar(*a, *counts);
        });
}

void on_sidebar_activated(PhigApp& app, GtkListBoxRow* row) {
    auto* scope = static_cast<phig::gtk::Scope*>(g_object_get_data(G_OBJECT(row), "phig-scope"));
    if (scope == nullptr) return;
    app.query.filter = phig::gtk::filter_for(*scope);
    query_changed(app);
}

GtkWidget* chip_new(PhigApp& app, phig::gtk::Chip chip) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), gtk_label_new(chip.label.c_str()));
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("window-close-symbolic"));
    GtkWidget* button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_add_css_class(button, "pill");
    gtk_widget_add_css_class(button, "small");
    gtk_widget_set_tooltip_text(button, "Remove this filter");
    PhigApp* a = &app;
    connect<void(GtkButton*)>(button, "clicked", [a, chip = std::move(chip)](GtkButton*) noexcept {
        phig::gtk::remove_chip(a->query.filter, chip);
        query_changed(*a);
    });
    return button;
}

void update_chips(PhigApp& app) {
    gtk_flow_box_remove_all(app.chips);
    std::vector<phig::gtk::Chip> chips = phig::gtk::chips(app.query.filter);
    for (phig::gtk::Chip& chip : chips)
        gtk_flow_box_append(app.chips, chip_new(app, std::move(chip)));
    gtk_widget_set_visible(GTK_WIDGET(app.chips), !chips.empty());
}

std::string entry_text(GtkEntry* entry) { return gtk_editable_get_text(GTK_EDITABLE(entry)); }

void set_entry_text(GtkEntry* entry, const std::string& text) {
    gtk_editable_set_text(GTK_EDITABLE(entry), text.c_str());
}

// Fills the popover's fields from the query when it opens.
void on_filter_shown(PhigApp& app) {
    const phig::gtk::FilterForm form = phig::gtk::form_for(app.query.filter);
    FilterFields& f = app.filter;
    set_entry_text(f.tag, form.tag);
    set_entry_text(f.gallery, form.gallery);
    gtk_check_button_set_active(f.favorite, form.favorite);
    set_entry_text(f.after, form.after);
    set_entry_text(f.before, form.before);
    set_entry_text(f.camera, form.camera);
    set_entry_text(f.make, form.make);
    guint media = 0;
    for (guint i = 0; i < G_N_ELEMENTS(kMedia); i++)
        if (form.media == kMedia[i]) media = i;
    gtk_drop_down_set_selected(f.media, media);
    set_entry_text(f.extension, form.extension);
}

void on_filter_apply(PhigApp& app) {
    FilterFields& f = app.filter;
    phig::gtk::FilterForm form;
    form.tag = entry_text(f.tag);
    form.gallery = entry_text(f.gallery);
    form.favorite = gtk_check_button_get_active(f.favorite) != FALSE;
    form.after = entry_text(f.after);
    form.before = entry_text(f.before);
    form.camera = entry_text(f.camera);
    form.make = entry_text(f.make);
    const guint media = gtk_drop_down_get_selected(f.media);
    form.media = media < G_N_ELEMENTS(kMedia) ? kMedia[media] : "";
    form.extension = entry_text(f.extension);
    auto applied = phig::gtk::apply_form(app.query.filter, form);
    if (!applied) {
        show_toast(app, applied.error());
        return;
    }
    gtk_menu_button_popdown(f.button);
    app.query.filter = std::move(*applied);
    query_changed(app);
}

// One labelled field of the filter popover.
GtkEntry* filter_entry(GtkGrid* grid, int row, const char* label, const char* placeholder) {
    GtkWidget* caption = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(caption), 1);
    GtkWidget* entry = gtk_entry_new();
    if (placeholder != nullptr) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_grid_attach(grid, caption, 0, row, 1, 1);
    gtk_grid_attach(grid, entry, 1, row, 1, 1);
    return GTK_ENTRY(entry);
}

GtkWidget* filter_button_new(PhigApp& app) {
    FilterFields& f = app.filter;
    GtkGrid* grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_row_spacing(grid, 6);
    gtk_grid_set_column_spacing(grid, 12);
    f.tag = filter_entry(grid, 0, "Add tag", "Has this tag too");
    f.gallery = filter_entry(grid, 1, "Gallery", nullptr);
    f.favorite = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Favorites only"));
    gtk_grid_attach(grid, GTK_WIDGET(f.favorite), 1, 2, 1, 1);
    f.after = filter_entry(grid, 3, "From", "YYYY-MM-DD");
    f.before = filter_entry(grid, 4, "Before", "YYYY-MM-DD");
    f.camera = filter_entry(grid, 5, "Camera", nullptr);
    f.make = filter_entry(grid, 6, "Make", nullptr);
    GtkWidget* media_caption = gtk_label_new("Media");
    gtk_label_set_xalign(GTK_LABEL(media_caption), 1);
    f.media = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kMediaNames));
    gtk_grid_attach(grid, media_caption, 0, 7, 1, 1);
    gtk_grid_attach(grid, GTK_WIDGET(f.media), 1, 7, 1, 1);
    f.extension = filter_entry(grid, 8, "Extension", "jpg");
    GtkWidget* apply = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_set_halign(apply, GTK_ALIGN_END);
    gtk_grid_attach(grid, apply, 1, 9, 1, 1);
    PhigApp* a = &app;
    connect<void(GtkButton*)>(apply, "clicked", [a](GtkButton*) noexcept { on_filter_apply(*a); });

    GtkWidget* popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), GTK_WIDGET(grid));
    connect<void(GtkPopover*)>(popover, "show", [a](GtkPopover*) noexcept { on_filter_shown(*a); });
    f.button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_label(f.button, "Filter");
    gtk_menu_button_set_popover(f.button, popover);
    return GTK_WIDGET(f.button);
}

void on_status_clicked(PhigApp& app) {
    if (app.status_kind != Status::NoMatches) return;
    app.query.filter = {};
    app.query.search.clear();
    // Clearing the entry emits search-changed, which finds the query already
    // matches it.
    gtk_editable_set_text(GTK_EDITABLE(app.search), "");
    query_changed(app);
}

std::vector<int64_t> selected_ids(PhigApp& app) {
    if (app.selection == nullptr || !app.model) return {};
    GtkBitset* set = gtk_selection_model_get_selection(app.selection);
    std::vector<int64_t> ids = phig_gallery_model_ids_in(app.model.get(), set);
    gtk_bitset_unref(set);
    return ids;
}

// A selection-bar action: a write over the selected images.
void write_selection(PhigApp& app,
                     std::move_only_function<phig::gtk::WriteResult(
                         phig::gtk::Collection&, const std::vector<int64_t>&) noexcept>
                         write) {
    std::vector<int64_t> ids = selected_ids(app);
    if (ids.empty()) return;
    run_write(app, [ids = std::move(ids), write = std::move(write)](
                       phig::gtk::Collection& coll) mutable noexcept { return write(coll, ids); });
}

GtkWidget* popover_button(const char* label, GtkWidget* content) {
    GtkWidget* popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), content);
    GtkWidget* button = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(button), label);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    return button;
}

// An entry and the buttons that act on its text, for the selection bar.
GtkWidget* entry_with_buttons(GtkEntry** entry, const char* placeholder,
                              std::vector<GtkWidget*> buttons) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    *entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(*entry, placeholder);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(*entry));
    for (GtkWidget* button : buttons) gtk_box_append(GTK_BOX(box), button);
    return box;
}

}  // namespace

void reload(PhigApp& app) {
    if (!app.coll) return;
    const guint seq = ++app.query_seq;
    phig::gtk::Collection* coll = app.coll.get();
    PhigApp* a = &app;
    phig::gtk::run_in_background<std::expected<std::vector<int64_t>, std::string>>(
        [coll, query = app.query]() noexcept { return phig::gtk::ids(*coll, query); },
        [a, seq](std::expected<std::vector<int64_t>, std::string> found) noexcept {
            if (a->window == nullptr || seq != a->query_seq) return;
            if (!found) {
                show_status(*a, Status::Error, found.error());
                return;
            }
            attach_model(*a, std::move(*found));
            if (!a->measured && g_getenv("PHIG_MEASURE") != nullptr) {
                a->measured = true;
                const gint64 elapsed_us = g_get_monotonic_time() - a->startup_start_us;
                g_printerr("PHIG_MEASURE startup_ms=%.3f\n", elapsed_us / 1000.0);
                phig_measure_maybe_start(a);
            }
        });
    refresh_counts(app);
}

void run_write(
    PhigApp& app,
    std::move_only_function<phig::gtk::WriteResult(phig::gtk::Collection&) noexcept> write) {
    if (!app.coll) return;
    phig::gtk::Collection* coll = app.coll.get();
    PhigApp* a = &app;
    phig::gtk::run_in_background<phig::gtk::WriteResult>(
        [coll, write = std::move(write)]() mutable noexcept { return write(*coll); },
        [a](phig::gtk::WriteResult result) noexcept {
            if (a->window == nullptr) return;
            if (!result) {
                show_toast(*a, phig::gtk::write_error_text(result.error()));
                return;
            }
            refresh_counts(*a);
            if (phig::gtk::selects_by_annotation(a->query.filter)) reload(*a);
            viewer_refresh(*a);
        });
}

GtkWidget* sidebar_new(PhigApp& app) {
    app.sidebar = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(app.sidebar, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(app.sidebar), "navigation-sidebar");
    PhigApp* a = &app;
    connect<void(GtkListBox*, GtkListBoxRow*)>(
        app.sidebar, "row-activated",
        [a](GtkListBox*, GtkListBoxRow* row) noexcept { on_sidebar_activated(*a, row); });
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(app.sidebar));
    return scroller;
}

void header_fill(PhigApp& app, AdwHeaderBar* header) {
    PhigApp* a = &app;
    app.search = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_search_entry_set_placeholder_text(app.search, "Search filenames and paths");
    gtk_widget_set_size_request(GTK_WIDGET(app.search), 320, -1);
    connect<void(GtkSearchEntry*)>(
        app.search, "search-changed", [a](GtkSearchEntry* entry) noexcept {
            std::string text = gtk_editable_get_text(GTK_EDITABLE(entry));
            if (text == a->query.search) return;
            a->query.search = std::move(text);
            reload(*a);
        });
    adw_header_bar_set_title_widget(header, GTK_WIDGET(app.search));

    app.sort = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kSortNames));
    gtk_widget_set_tooltip_text(GTK_WIDGET(app.sort), "Sort");
    connect<void(GObject*, GParamSpec*)>(app.sort, "notify::selected",
                                         [a](GObject*, GParamSpec*) noexcept {
                                             const guint i = gtk_drop_down_get_selected(a->sort);
                                             if (i >= G_N_ELEMENTS(kSorts)) return;
                                             a->query.sort = kSorts[i];
                                             reload(*a);
                                         });
    adw_header_bar_pack_end(header, filter_button_new(app));
    adw_header_bar_pack_end(header, GTK_WIDGET(app.sort));
}

GtkWidget* chips_new(PhigApp& app) {
    app.chips = GTK_FLOW_BOX(gtk_flow_box_new());
    gtk_flow_box_set_selection_mode(app.chips, GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(app.chips, 20);
    gtk_widget_set_margin_start(GTK_WIDGET(app.chips), 6);
    gtk_widget_set_margin_end(GTK_WIDGET(app.chips), 6);
    gtk_widget_set_visible(GTK_WIDGET(app.chips), FALSE);
    return GTK_WIDGET(app.chips);
}

GtkWidget* status_new(PhigApp& app) {
    app.status = ADW_STATUS_PAGE(adw_status_page_new());
    app.status_button = GTK_BUTTON(gtk_button_new());
    gtk_widget_set_halign(GTK_WIDGET(app.status_button), GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(GTK_WIDGET(app.status_button), "pill");
    gtk_widget_add_css_class(GTK_WIDGET(app.status_button), "suggested-action");
    PhigApp* a = &app;
    connect<void(GtkButton*)>(app.status_button, "clicked",
                              [a](GtkButton*) noexcept { on_status_clicked(*a); });
    adw_status_page_set_child(app.status, GTK_WIDGET(app.status_button));
    return GTK_WIDGET(app.status);
}

void show_status(PhigApp& app, Status status, const std::string& message) {
    app.status_kind = status;
    const char* icon = "image-x-generic-symbolic";
    const char* title = "No Photos Yet";
    std::string description = "Scan a folder to add its photos to the collection.";
    const char* button = nullptr;
    if (status == Status::NoMatches) {
        icon = "edit-find-symbolic";
        title = "No Matches";
        description = "No photos match these filters.";
        button = "Clear Filters";
    } else if (status == Status::Error) {
        icon = "dialog-error-symbolic";
        title = "Could Not Open Collection";
        description = message;
        button = nullptr;
    }
    adw_status_page_set_icon_name(app.status, icon);
    adw_status_page_set_title(app.status, title);
    char* escaped = g_markup_escape_text(description.c_str(), -1);
    adw_status_page_set_description(app.status, escaped);
    g_free(escaped);
    gtk_widget_set_visible(GTK_WIDGET(app.status_button), button != nullptr);
    if (button != nullptr) gtk_button_set_label(app.status_button, button);
    gtk_stack_set_visible_child_name(app.content, "status");
}

void browse_set_sensitive(PhigApp& app, bool sensitive) {
    gtk_widget_set_sensitive(GTK_WIDGET(app.search), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app.sort), sensitive);
    gtk_widget_set_sensitive(GTK_WIDGET(app.filter.button), sensitive);
}

GtkWidget* selection_bar_new(PhigApp& app) {
    PhigApp* a = &app;
    app.selection_bar = GTK_ACTION_BAR(gtk_action_bar_new());
    app.selection_label = GTK_LABEL(gtk_label_new(nullptr));
    gtk_action_bar_pack_start(app.selection_bar, GTK_WIDGET(app.selection_label));

    GtkWidget* add_tag = gtk_button_new_with_label("Add");
    GtkWidget* remove_tag = gtk_button_new_with_label("Remove");
    connect<void(GtkButton*)>(add_tag, "clicked", [a](GtkButton*) noexcept {
        write_selection(
            *a, [tag = entry_text(a->selection_tag)](phig::gtk::Collection& coll,
                                                     const std::vector<int64_t>& ids) noexcept {
                return phig::gtk::tag_add(coll, ids, tag);
            });
    });
    connect<void(GtkButton*)>(remove_tag, "clicked", [a](GtkButton*) noexcept {
        write_selection(
            *a, [tag = entry_text(a->selection_tag)](phig::gtk::Collection& coll,
                                                     const std::vector<int64_t>& ids) noexcept {
                return phig::gtk::tag_remove(coll, ids, tag);
            });
    });
    GtkWidget* tags = entry_with_buttons(&app.selection_tag, "Tag name", {add_tag, remove_tag});

    GtkWidget* set_gallery = gtk_button_new_with_label("Set");
    gtk_widget_set_tooltip_text(set_gallery, "Replaces any gallery the photos are in");
    GtkWidget* clear_gallery = gtk_button_new_with_label("Clear");
    connect<void(GtkButton*)>(set_gallery, "clicked", [a](GtkButton*) noexcept {
        write_selection(*a,
                        [gallery = entry_text(a->selection_gallery)](
                            phig::gtk::Collection& coll, const std::vector<int64_t>& ids) noexcept {
                            return phig::gtk::gallery_set(coll, ids, gallery);
                        });
    });
    connect<void(GtkButton*)>(clear_gallery, "clicked", [a](GtkButton*) noexcept {
        write_selection(*a,
                        [](phig::gtk::Collection& coll, const std::vector<int64_t>& ids) noexcept {
                            return phig::gtk::gallery_clear(coll, ids);
                        });
    });
    GtkWidget* galleries =
        entry_with_buttons(&app.selection_gallery, "Gallery name", {set_gallery, clear_gallery});

    GtkWidget* favorite = gtk_button_new_from_icon_name("starred-symbolic");
    gtk_widget_set_tooltip_text(favorite, "Add to Favorites");
    gtk_accessible_update_property(GTK_ACCESSIBLE(favorite), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Add to Favorites", -1);
    connect<void(GtkButton*)>(favorite, "clicked", [a](GtkButton*) noexcept {
        write_selection(*a,
                        [](phig::gtk::Collection& coll, const std::vector<int64_t>& ids) noexcept {
                            return phig::gtk::favorite_set(coll, ids, true);
                        });
    });
    GtkWidget* unfavorite = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_set_tooltip_text(unfavorite, "Remove from Favorites");
    gtk_accessible_update_property(GTK_ACCESSIBLE(unfavorite), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Remove from Favorites", -1);
    connect<void(GtkButton*)>(unfavorite, "clicked", [a](GtkButton*) noexcept {
        write_selection(*a,
                        [](phig::gtk::Collection& coll, const std::vector<int64_t>& ids) noexcept {
                            return phig::gtk::favorite_set(coll, ids, false);
                        });
    });
    GtkWidget* deselect = gtk_button_new_with_label("Select None");
    connect<void(GtkButton*)>(deselect, "clicked", [a](GtkButton*) noexcept {
        if (a->selection != nullptr) gtk_selection_model_unselect_all(a->selection);
    });

    gtk_action_bar_pack_end(app.selection_bar, deselect);
    gtk_action_bar_pack_end(app.selection_bar, unfavorite);
    gtk_action_bar_pack_end(app.selection_bar, favorite);
    gtk_action_bar_pack_end(app.selection_bar, popover_button("Gallery…", galleries));
    gtk_action_bar_pack_end(app.selection_bar, popover_button("Tag…", tags));
    gtk_action_bar_set_revealed(app.selection_bar, FALSE);
    return GTK_WIDGET(app.selection_bar);
}

void update_selection_bar(PhigApp& app) {
    if (app.window == nullptr) return;
    guint64 n = 0;
    if (app.selection != nullptr) {
        GtkBitset* set = gtk_selection_model_get_selection(app.selection);
        n = gtk_bitset_get_size(set);
        gtk_bitset_unref(set);
    }
    gtk_label_set_text(app.selection_label, phig::gtk::selection_text(n).c_str());
    gtk_action_bar_set_revealed(app.selection_bar, n > 0);
}
