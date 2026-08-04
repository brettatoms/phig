#include <adwaita.h>
#include <gtk/gtk.h>

static void on_activate(GtkApplication* app, gpointer user_data) {
    (void)user_data;
    GtkWidget* window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "phig");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 750);

    AdwToolbarView* view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, adw_header_bar_new());
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), GTK_WIDGET(view));

    gtk_window_present(GTK_WINDOW(window));
}

int main(int argc, char** argv) {
    AdwApplication* app =
        adw_application_new("org.phig.Gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
