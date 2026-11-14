#include <expected>
#include <string>
#include <utility>

#include "app.h"
#include "connect.h"
#include "format.h"

namespace {

using phig::gtk::connect;

// The scan dialog's state, owned by the dialog.
struct ScanForm {
    PhigApp* app = nullptr;
    AdwAlertDialog* dialog = nullptr;
    GtkButton* folder_button = nullptr;
    GtkCheckButton* recursive = nullptr;
    GtkCheckButton* hidden = nullptr;
    std::string folder;
};

ScanForm& form_of(AdwAlertDialog* dialog) {
    return *static_cast<ScanForm*>(g_object_get_data(G_OBJECT(dialog), "phig-scan-form"));
}

bool cancelling(const PhigApp& app) {
    return app.scan_cancel_button != nullptr &&
           !gtk_widget_get_sensitive(GTK_WIDGET(app.scan_cancel_button));
}

void scan_finished(PhigApp& app, std::expected<phig::gtk::IndexSummary, std::string> result) {
    // The watch is idle once its on_done runs, and may be destroyed here.
    app.scan.reset();
    if (app.window == nullptr) return;
    gtk_widget_set_visible(app.scan_bar, FALSE);
    scan_update_sensitivity(app);
    show_toast(app, result ? phig::gtk::summary_text(*result) : "Scan failed: " + result.error());
    if (app.coll)
        reload(app);
    else
        open_catalog(app);  // a scan creates a missing catalog
}

void scan_progressed(PhigApp& app, const phig::gtk::IndexProgress& p) {
    if (app.window == nullptr || cancelling(app)) return;
    const std::string text = "Scanning " + app.scan_folder + " — " + phig::gtk::progress_text(p);
    gtk_label_set_text(app.scan_label, text.c_str());
    if (p.total > 0)
        gtk_progress_bar_set_fraction(app.scan_progress,
                                      static_cast<double>(p.done) / static_cast<double>(p.total));
    else
        gtk_progress_bar_pulse(app.scan_progress);
}

void start_scan(PhigApp& app, phig::gtk::IndexOptions options) {
    if (app.scan) return;
    PhigApp* a = &app;
    std::string folder = options.directory;
    auto watch = phig::gtk::scan_start(
        app.catalog_dir, std::move(options),
        [a](const phig::gtk::IndexProgress& p) noexcept { scan_progressed(*a, p); },
        [a](std::expected<phig::gtk::IndexSummary, std::string> result) noexcept {
            scan_finished(*a, std::move(result));
        });
    if (!watch) {
        show_toast(app, "Could not start the scan: " + watch.error());
        return;
    }
    app.scan = std::move(*watch);
    app.scan_folder = std::move(folder);
    scan_progressed(app, {});
    gtk_progress_bar_set_fraction(app.scan_progress, 0.0);
    gtk_widget_set_visible(app.scan_bar, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app.scan_cancel_button), TRUE);
    scan_update_sensitivity(app);
}

void on_folder_chosen(GObject* source, GAsyncResult* result, gpointer data) noexcept {
    GRef<AdwAlertDialog> dialog = adopt(static_cast<AdwAlertDialog*>(data));
    GFile* file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
    if (file == nullptr) return;  // dismissed
    char* path = g_file_get_path(file);
    g_object_unref(file);
    if (path == nullptr) return;
    ScanForm& form = form_of(dialog.get());
    form.folder = path;
    g_free(path);
    gtk_button_set_label(form.folder_button, form.folder.c_str());
    adw_alert_dialog_set_response_enabled(dialog.get(), "scan", TRUE);
}

void choose_folder(ScanForm& form) {
    GtkFileDialog* chooser = gtk_file_dialog_new();
    gtk_file_dialog_set_title(chooser, "Choose a Folder to Scan");
    gtk_file_dialog_select_folder(chooser, GTK_WINDOW(form.app->window), nullptr, on_folder_chosen,
                                  g_object_ref(form.dialog));
    g_object_unref(chooser);
}

}  // namespace

GtkWidget* scan_bar_new(PhigApp& app) {
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(bar, "toolbar");
    app.scan_label = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_xalign(app.scan_label, 0);
    gtk_label_set_ellipsize(app.scan_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(app.scan_label), TRUE);
    app.scan_progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_size_request(GTK_WIDGET(app.scan_progress), 200, -1);
    gtk_widget_set_valign(GTK_WIDGET(app.scan_progress), GTK_ALIGN_CENTER);
    GtkWidget* cancel = gtk_button_new_with_label("Cancel");
    app.scan_cancel_button = GTK_BUTTON(cancel);
    PhigApp* a = &app;
    connect<void(GtkButton*)>(cancel, "clicked", [a](GtkButton*) noexcept {
        if (!a->scan) return;
        phig::gtk::scan_cancel(*a->scan);
        gtk_widget_set_sensitive(GTK_WIDGET(a->scan_cancel_button), FALSE);
        gtk_label_set_text(a->scan_label, "Cancelling…");
    });
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(app.scan_label));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(app.scan_progress));
    gtk_box_append(GTK_BOX(bar), cancel);
    gtk_widget_set_visible(bar, FALSE);
    app.scan_bar = bar;
    return bar;
}

void scan_update_sensitivity(PhigApp& app) {
    const gboolean on = !app.scan && !app.opening;
    if (app.scan_button != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(app.scan_button), on);
    if (app.status_button != nullptr) gtk_widget_set_sensitive(GTK_WIDGET(app.status_button), on);
}

void scan_dialog_present(PhigApp& app) {
    if (app.scan) return;
    AdwAlertDialog* dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Scan a Folder",
        "Adds new and changed photos and videos to the collection. It never removes a "
        "photo from the collection, even when its file is gone."));
    auto* form = new ScanForm{};
    form->app = &app;
    form->dialog = dialog;
    g_object_set_data_full(G_OBJECT(dialog), "phig-scan-form", form,
                           [](gpointer p) noexcept { delete static_cast<ScanForm*>(p); });

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    form->folder_button = GTK_BUTTON(gtk_button_new_with_label("Choose Folder…"));
    connect<void(GtkButton*)>(form->folder_button, "clicked",
                              [form](GtkButton*) noexcept { choose_folder(*form); });
    form->recursive = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Include subfolders"));
    gtk_check_button_set_active(form->recursive, TRUE);
    form->hidden =
        GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Include hidden files and folders"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->folder_button));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->recursive));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(form->hidden));
    adw_alert_dialog_set_extra_child(dialog, box);

    adw_alert_dialog_add_responses(dialog, "cancel", "_Cancel", "scan", "_Scan", nullptr);
    adw_alert_dialog_set_response_appearance(dialog, "scan", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_enabled(dialog, "scan", FALSE);
    adw_alert_dialog_set_default_response(dialog, "scan");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    connect<void(AdwAlertDialog*, const char*)>(
        dialog, "response", [](AdwAlertDialog* d, const char* response) noexcept {
            if (g_strcmp0(response, "scan") != 0) return;
            ScanForm& f = form_of(d);
            start_scan(*f.app, {f.folder, gtk_check_button_get_active(f.recursive) != FALSE,
                                gtk_check_button_get_active(f.hidden) != FALSE});
        });
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(app.window));
}
