#include <expected>
#include <utility>

#include "app.h"
#include "background.h"
#include "connect.h"
#include "format.h"

namespace {

using phig::gtk::connect;

// Longest edge the viewer decodes to. A 4096 x 2731 texture is 33 MB, and the
// viewer holds one at a time.
constexpr int kViewerMaxEdge = 4096;

// What the thumbnail request asks for: the size the grid's cache already has.
constexpr guint32 kViewerThumbSize = PHIG_THUMB_SIZE_DEFAULT;

void set_text(GtkLabel* label, const std::string& text) {
    gtk_label_set_text(label, text.empty() ? "—" : text.c_str());
}

GtkWidget* tag_chip(PhigApp& app, const std::string& tag) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(box), gtk_label_new(tag.c_str()));
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name("window-close-symbolic"));
    GtkWidget* button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_add_css_class(button, "pill");
    gtk_widget_add_css_class(button, "small");
    gtk_widget_set_tooltip_text(button, "Remove this tag");
    PhigApp* a = &app;
    connect<void(GtkButton*)>(button, "clicked", [a, tag](GtkButton*) noexcept {
        run_write(*a, [id = a->viewer.id, tag](phig::gtk::Collection& coll) noexcept {
            return phig::gtk::tag_remove(coll, {id}, tag);
        });
    });
    return button;
}

// keep_gallery leaves the gallery entry as it is, for a refresh of the same
// image while the user is typing in it.
void show_details(PhigApp& app, bool keep_gallery) {
    Viewer& v = app.viewer;
    const phig::gtk::Details& d = v.details;
    adw_navigation_page_set_title(v.page, d.filename.c_str());
    set_text(v.filename, d.filename);
    set_text(v.date, d.date);
    set_text(v.camera, phig::gtk::camera_text(d));
    set_text(v.exposure, phig::gtk::exposure_text(d));
    set_text(v.size, phig::gtk::size_text(d));
    set_text(v.sharpness, phig::gtk::sharpness_text(d));
    gtk_flow_box_remove_all(v.tags);
    for (const std::string& tag : d.tags) gtk_flow_box_append(v.tags, tag_chip(app, tag));
    if (!keep_gallery) gtk_editable_set_text(GTK_EDITABLE(v.gallery_entry), d.gallery.c_str());
    gtk_button_set_icon_name(v.favorite, d.favorite ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(v.favorite),
                                d.favorite ? "Remove from Favorites" : "Add to Favorites");
}

// True when the keyboard focus is in the gallery entry (its inner text widget).
bool gallery_entry_focused(const Viewer& v) {
    GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(v.gallery_entry));
    GtkWidget* focus = root != nullptr ? gtk_root_get_focus(root) : nullptr;
    return focus != nullptr && (focus == GTK_WIDGET(v.gallery_entry) ||
                                gtk_widget_is_ancestor(focus, GTK_WIDGET(v.gallery_entry)));
}

void clear_details(Viewer& v) {
    adw_navigation_page_set_title(v.page, "Photo");
    gtk_button_set_icon_name(v.favorite, "non-starred-symbolic");
    gtk_widget_set_tooltip_text(GTK_WIDGET(v.favorite), "Add to Favorites");
    for (GtkLabel* label : {v.filename, v.date, v.camera, v.exposure, v.size, v.sharpness})
        gtk_label_set_text(label, "");
    gtk_flow_box_remove_all(v.tags);
    gtk_editable_set_text(GTK_EDITABLE(v.tag_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(v.gallery_entry), "");
}

// Shows the thumbnail while the decode is still running.
void request_thumbnail(PhigApp& app) {
    Viewer& v = app.viewer;
    if (!app.loader || v.full_shown || v.details.sha256.empty()) return;
    v.thumb_cancel = adopt(g_cancellable_new());
    PhigApp* a = &app;
    const guint seq = v.seq;
    thumb_loader_request(*app.loader, v.id, v.details.sha256, kViewerThumbSize,
                         v.thumb_cancel.get(),
                         [a, seq](ThumbResult result, GdkTexture* texture) noexcept {
                             Viewer& viewer = a->viewer;
                             if (a->window == nullptr || seq != viewer.seq || viewer.full_shown)
                                 return;
                             if (result == THUMB_OK)
                                 gtk_picture_set_paintable(viewer.picture, GDK_PAINTABLE(texture));
                         });
}

void load_details(PhigApp& app) {
    phig::gtk::Collection* coll = app.coll.get();
    PhigApp* a = &app;
    const guint seq = app.viewer.seq;
    const guint details_seq = ++app.viewer.details_seq;
    phig::gtk::run_in_background<std::expected<phig::gtk::Details, std::string>>(
        [coll, id = app.viewer.id]() noexcept { return phig::gtk::details(*coll, id); },
        [a, seq, details_seq](std::expected<phig::gtk::Details, std::string> found) noexcept {
            if (a->window == nullptr || seq != a->viewer.seq ||
                details_seq != a->viewer.details_seq)
                return;
            if (!found) {
                show_toast(*a, found.error());
                return;
            }
            const bool first = !a->viewer.details_loaded;
            a->viewer.details = std::move(*found);
            a->viewer.details_loaded = true;
            // With no write running or queued, the details are the image's state.
            if (!a->write_running) a->viewer.favorite_requested.reset();
            show_details(*a, !first && gallery_entry_focused(a->viewer));
            if (first) request_thumbnail(*a);
        });
}

// Keeps one decode in flight. A decode that lands for an older image is
// dropped and one for the current image starts in its place.
void load_image(PhigApp& app) {
    if (app.viewer.decode_running) return;
    app.viewer.decode_running = true;
    phig::gtk::Collection* coll = app.coll.get();
    PhigApp* a = &app;
    const guint seq = app.viewer.seq;
    phig::gtk::run_in_background<std::expected<GRef<GdkTexture>, std::string>>(
        [coll, id = app.viewer.id]() noexcept {
            return phig::gtk::image(*coll, id, kViewerMaxEdge);
        },
        [a, seq](std::expected<GRef<GdkTexture>, std::string> decoded) noexcept {
            a->viewer.decode_running = false;
            if (a->window == nullptr) return;
            if (seq != a->viewer.seq) {
                if (a->viewer.model) load_image(*a);
                return;
            }
            if (!decoded) {
                show_toast(*a, decoded.error());
                return;
            }
            a->viewer.full_shown = true;
            if (a->viewer.thumb_cancel) g_cancellable_cancel(a->viewer.thumb_cancel.get());
            gtk_picture_set_paintable(a->viewer.picture, GDK_PAINTABLE(decoded->get()));
        });
}

void show_position(PhigApp& app, guint position) {
    Viewer& v = app.viewer;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(v.model.get()));
    if (position >= n) return;
    if (v.thumb_cancel) g_cancellable_cancel(v.thumb_cancel.get());
    v.position = position;
    v.id = phig_gallery_model_id_at(v.model.get(), position);
    v.seq++;
    v.full_shown = false;
    v.details_loaded = false;
    v.details = {};
    v.favorite_requested.reset();
    gtk_picture_set_paintable(v.picture, nullptr);
    clear_details(v);
    gtk_widget_set_sensitive(GTK_WIDGET(v.prev), position > 0);
    gtk_widget_set_sensitive(GTK_WIDGET(v.next), position + 1 < n);
    load_details(app);
    load_image(app);
}

void step(PhigApp& app, int delta) {
    Viewer& v = app.viewer;
    if (!v.model) return;
    if (delta < 0 && v.position == 0) return;
    show_position(app, delta < 0 ? v.position - 1 : v.position + 1);
}

// A caption above a value, for the metadata pane. The value is not selectable:
// a selectable label takes focus, and then the arrow keys move its cursor
// instead of stepping through the photos.
GtkLabel* pane_field(GtkBox* pane, const char* caption) {
    GtkWidget* title = gtk_label_new(caption);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_widget_add_css_class(title, "caption-heading");
    gtk_widget_add_css_class(title, "dim-label");
    GtkWidget* value = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_wrap(GTK_LABEL(value), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(value), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(pane, title);
    gtk_box_append(pane, value);
    return GTK_LABEL(value);
}

GtkWidget* pane_new(PhigApp& app) {
    Viewer& v = app.viewer;
    PhigApp* a = &app;
    GtkBox* pane = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 6));
    gtk_widget_set_margin_start(GTK_WIDGET(pane), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(pane), 12);
    gtk_widget_set_margin_top(GTK_WIDGET(pane), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(pane), 12);
    v.filename = pane_field(pane, "Filename");
    v.date = pane_field(pane, "Date");
    v.camera = pane_field(pane, "Camera");
    v.exposure = pane_field(pane, "Exposure");
    v.size = pane_field(pane, "Size");
    v.sharpness = pane_field(pane, "Sharpness");

    GtkWidget* tags_title = gtk_label_new("Tags");
    gtk_label_set_xalign(GTK_LABEL(tags_title), 0);
    gtk_widget_add_css_class(tags_title, "caption-heading");
    gtk_widget_add_css_class(tags_title, "dim-label");
    gtk_box_append(pane, tags_title);
    v.tags = GTK_FLOW_BOX(gtk_flow_box_new());
    gtk_flow_box_set_selection_mode(v.tags, GTK_SELECTION_NONE);
    gtk_box_append(pane, GTK_WIDGET(v.tags));
    GtkWidget* tag_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    v.tag_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(v.tag_entry, "Add a tag");
    gtk_widget_set_hexpand(GTK_WIDGET(v.tag_entry), TRUE);
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Add Tag");
    auto add_tag = [a]() noexcept {
        std::string tag = gtk_editable_get_text(GTK_EDITABLE(a->viewer.tag_entry));
        gtk_editable_set_text(GTK_EDITABLE(a->viewer.tag_entry), "");
        run_write(*a,
                  [id = a->viewer.id, tag = std::move(tag)](phig::gtk::Collection& coll) noexcept {
                      return phig::gtk::tag_add(coll, {id}, tag);
                  });
    };
    connect<void(GtkButton*)>(add, "clicked", [add_tag](GtkButton*) noexcept { add_tag(); });
    connect<void(GtkEntry*)>(v.tag_entry, "activate", [add_tag](GtkEntry*) noexcept { add_tag(); });
    gtk_box_append(GTK_BOX(tag_row), GTK_WIDGET(v.tag_entry));
    gtk_box_append(GTK_BOX(tag_row), add);
    gtk_box_append(pane, tag_row);

    GtkWidget* gallery_title = gtk_label_new("Gallery");
    gtk_label_set_xalign(GTK_LABEL(gallery_title), 0);
    gtk_widget_add_css_class(gallery_title, "caption-heading");
    gtk_widget_add_css_class(gallery_title, "dim-label");
    gtk_box_append(pane, gallery_title);
    GtkWidget* gallery_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    v.gallery_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(v.gallery_entry, "None");
    gtk_widget_set_hexpand(GTK_WIDGET(v.gallery_entry), TRUE);
    GtkWidget* set = gtk_button_new_with_label("Set");
    GtkWidget* clear = gtk_button_new_with_label("Clear");
    auto set_gallery = [a]() noexcept {
        std::string gallery = gtk_editable_get_text(GTK_EDITABLE(a->viewer.gallery_entry));
        run_write(*a, [id = a->viewer.id,
                       gallery = std::move(gallery)](phig::gtk::Collection& coll) noexcept {
            return phig::gtk::gallery_set(coll, {id}, gallery);
        });
    };
    connect<void(GtkButton*)>(set, "clicked",
                              [set_gallery](GtkButton*) noexcept { set_gallery(); });
    connect<void(GtkEntry*)>(v.gallery_entry, "activate",
                             [set_gallery](GtkEntry*) noexcept { set_gallery(); });
    connect<void(GtkButton*)>(clear, "clicked", [a](GtkButton*) noexcept {
        run_write(*a, [id = a->viewer.id](phig::gtk::Collection& coll) noexcept {
            return phig::gtk::gallery_clear(coll, {id});
        });
    });
    gtk_box_append(GTK_BOX(gallery_row), GTK_WIDGET(v.gallery_entry));
    gtk_box_append(GTK_BOX(gallery_row), set);
    gtk_box_append(GTK_BOX(gallery_row), clear);
    gtk_box_append(pane, gallery_row);

    GtkWidget* open = gtk_button_new_with_label("Open in System Viewer");
    connect<void(GtkButton*)>(open, "clicked", [a](GtkButton*) noexcept {
        if (!a->viewer.details.path.empty()) launch_file(*a, a->viewer.details.path);
    });
    GtkWidget* show = gtk_button_new_with_label("Show in File Manager");
    connect<void(GtkButton*)>(show, "clicked", [a](GtkButton*) noexcept {
        if (a->viewer.details.path.empty()) return;
        GFile* file = g_file_new_for_path(a->viewer.details.path.c_str());
        GtkFileLauncher* launcher = gtk_file_launcher_new(file);
        g_object_unref(file);
        gtk_file_launcher_open_containing_folder(
            launcher, GTK_WINDOW(a->window), nullptr,
            [](GObject* source, GAsyncResult* result, gpointer data) noexcept {
                GError* error = nullptr;
                if (!gtk_file_launcher_open_containing_folder_finish(GTK_FILE_LAUNCHER(source),
                                                                     result, &error)) {
                    auto* app = static_cast<PhigApp*>(data);
                    if (app->window != nullptr) show_toast(*app, error->message);
                    g_error_free(error);
                }
                g_object_unref(source);
            },
            a);
    });
    gtk_widget_set_margin_top(open, 12);
    gtk_box_append(pane, open);
    gtk_box_append(pane, show);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(pane));
    gtk_widget_set_size_request(scroller, 300, -1);
    /* Set, not computed: the entries inside expand, and a computed hexpand
     * would let the pane take half the width from the picture. */
    gtk_widget_set_hexpand(scroller, FALSE);
    return scroller;
}

gboolean on_left(GtkWidget*, GVariant*, gpointer data) noexcept {
    step(*static_cast<PhigApp*>(data), -1);
    return TRUE;
}

gboolean on_right(GtkWidget*, GVariant*, gpointer data) noexcept {
    step(*static_cast<PhigApp*>(data), 1);
    return TRUE;
}

void add_shortcut(GtkShortcutController* controller, guint key, GtkShortcutFunc fn, PhigApp& app) {
    gtk_shortcut_controller_add_shortcut(
        controller, gtk_shortcut_new(gtk_keyval_trigger_new(key, GdkModifierType(0)),
                                     gtk_callback_action_new(fn, &app, nullptr)));
}

}  // namespace

void viewer_build(PhigApp& app) {
    Viewer& v = app.viewer;
    PhigApp* a = &app;
    AdwHeaderBar* header = ADW_HEADER_BAR(adw_header_bar_new());
    v.prev = GTK_BUTTON(gtk_button_new_from_icon_name("go-previous-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v.prev), "Previous");
    v.next = GTK_BUTTON(gtk_button_new_from_icon_name("go-next-symbolic"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(v.next), "Next");
    connect<void(GtkButton*)>(v.prev, "clicked", [a](GtkButton*) noexcept { step(*a, -1); });
    connect<void(GtkButton*)>(v.next, "clicked", [a](GtkButton*) noexcept { step(*a, 1); });
    adw_header_bar_pack_start(header, GTK_WIDGET(v.prev));
    adw_header_bar_pack_start(header, GTK_WIDGET(v.next));
    v.favorite = GTK_BUTTON(gtk_button_new_from_icon_name("non-starred-symbolic"));
    connect<void(GtkButton*)>(v.favorite, "clicked", [a](GtkButton*) noexcept {
        Viewer& viewer = a->viewer;
        if (!viewer.details_loaded) return;
        const bool on = !viewer.favorite_requested.value_or(viewer.details.favorite);
        viewer.favorite_requested = on;
        run_write(*a, [id = viewer.id, on](phig::gtk::Collection& coll) noexcept {
            return phig::gtk::favorite_set(coll, {id}, on);
        });
    });
    adw_header_bar_pack_end(header, GTK_WIDGET(v.favorite));

    v.picture = GTK_PICTURE(gtk_picture_new());
    gtk_picture_set_content_fit(v.picture, GTK_CONTENT_FIT_CONTAIN);
    /* Focus starts on the picture, so the arrow keys step through photos
     * rather than move the cursor in the tag entry. */
    gtk_widget_set_focusable(GTK_WIDGET(v.picture), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(v.picture), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(v.picture), TRUE);

    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(body), GTK_WIDGET(v.picture));
    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(body), pane_new(app));

    GtkWidget* toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(header));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), body);

    v.page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(toolbar, "Photo"));
    adw_navigation_page_set_tag(v.page, "viewer");
    GtkEventController* keys = gtk_shortcut_controller_new();
    add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), GDK_KEY_Left, on_left, app);
    add_shortcut(GTK_SHORTCUT_CONTROLLER(keys), GDK_KEY_Right, on_right, app);
    gtk_widget_add_controller(GTK_WIDGET(v.page), keys);
    connect<void(AdwNavigationPage*)>(v.page, "shown", [a](AdwNavigationPage*) noexcept {
        gtk_widget_grab_focus(GTK_WIDGET(a->viewer.picture));
    });
    // Dropping the model when the page is popped lets a gallery reload free the
    // old one.
    connect<void(AdwNavigationPage*)>(v.page, "hidden", [a](AdwNavigationPage*) noexcept {
        if (a->window == nullptr) return;
        Viewer& viewer = a->viewer;
        if (viewer.thumb_cancel) g_cancellable_cancel(viewer.thumb_cancel.get());
        viewer.seq++;
        viewer.model.reset();
        gtk_picture_set_paintable(viewer.picture, nullptr);
    });
    adw_navigation_view_add(app.nav, v.page);
}

void viewer_open(PhigApp& app, guint position) {
    if (!app.model || !app.coll) return;
    app.viewer.model = gref(app.model.get());
    show_position(app, position);
    adw_navigation_view_push_by_tag(app.nav, "viewer");
}

void viewer_refresh(PhigApp& app) {
    Viewer& v = app.viewer;
    if (!v.model || !app.coll) return;
    load_details(app);
}
