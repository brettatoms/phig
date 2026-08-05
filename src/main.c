#include <adwaita.h>
#include <gtk/gtk.h>
#include <phig/phig.h>

#include "gallery-model.h"
#include "thumb-loader.h"

/* Cell size in the grid, and the thumbnail cache budget. Task 7 measures
 * these against real scrolling; these are starting values, not tuned ones. */
#define PHIG_CELL_SIZE 200
#define PHIG_THUMB_LOADER_THREADS 4
#define PHIG_THUMB_CACHE_BYTES (256 * 1024 * 1024)

typedef struct {
    AdwApplicationWindow* window;
    AdwToolbarView* toolbar_view;
    GtkGridView* grid;
    AdwToastOverlay* toast_overlay;
    GtkLabel* failed_label; /* header-bar counter for THUMB_FAILED results */
    char* db_path;

    /* Owned once startup finishes; NULL until then. Kept on the app struct so
     * shutdown can free them in order (loader before ctx: workers hold ctx
     * for as long as they run). */
    phig_ctx* ctx;
    ThumbLoader* loader;

    guint thumb_failed_count;
    gboolean page_fetch_toast_shown; /* one toast per session, not one per failed page */
} PhigApp;

static void phig_app_free(PhigApp* app) {
    g_clear_object(&app->loader);
    if (app->ctx != NULL)
        phig_shutdown(app->ctx);
    g_free(app->db_path);
    g_free(app);
}

/* Callback for gtk_file_launcher_launch async completion. On error, shows a
 * toast with the error message; on success, the system viewer has been started. */
static void on_launched(GObject* source, GAsyncResult* result, gpointer user_data) {
    (void)source;
    GtkFileLauncher* launcher = GTK_FILE_LAUNCHER(source);
    PhigApp* app = user_data;
    GError* error = NULL;

    if (!gtk_file_launcher_launch_finish(launcher, result, &error)) {
        const char* message = error != NULL ? error->message : "Unknown error";
        AdwToast* toast = adw_toast_new(message);
        adw_toast_overlay_add_toast(app->toast_overlay, toast);
        if (error != NULL)
            g_error_free(error);
    }

    g_object_unref(launcher);
}

/* Result of the worker startup sequence, handed to the main thread. On
 * failure ctx/query are NULL and error explains why. */
typedef struct {
    PhigApp* app; /* borrowed; app outlives this, freed only at process exit */
    phig_ctx* ctx;
    phig_query* query;
} StartupResult;

/* phig_last_error reports the last error on the calling thread, so it must be
 * read here, on the worker thread that made the failing call, and carried to
 * the main thread as an owned copy — by the time startup_done runs on the
 * main thread the ctx may have been used again (or never existed at all) and
 * the message would be stale or unreadable. A failed phig_init produces no
 * context at all, per the header, so that case carries a fixed message
 * instead of a phig_last_error call. */
static void startup_thread_func(GTask* task, gpointer source_object, gpointer task_data,
                                 GCancellable* cancellable) {
    (void)source_object;
    (void)cancellable;
    StartupResult* result = task_data;

    phig_config cfg = {0};
    cfg.db_path = result->app->db_path;
    cfg.mode = PHIG_READ_ONLY;

    phig_status status = phig_init(&cfg, &result->ctx);
    if (status != PHIG_OK) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Could not open the phig database.");
        return;
    }

    status = phig_query_new(result->ctx, &result->query);
    if (status != PHIG_OK) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
                                 phig_last_error(result->ctx));
        return;
    }
    phig_query_set_sort(result->query, PHIG_SORT_DATE, TRUE);

    g_task_return_boolean(task, TRUE);
}

/* setup: build the fixed-size cell. content-fit COVER keeps thumbnails from
 * distorting; the frame gives every cell the same footprint whether or not
 * it currently has a picture. */
static void on_factory_setup(GtkSignalListItemFactory* factory, GtkListItem* item,
                              gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget* frame = gtk_frame_new(NULL);
    gtk_widget_set_size_request(frame, PHIG_CELL_SIZE, PHIG_CELL_SIZE);

    GtkWidget* picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_add_css_class(picture, "phig-thumb-placeholder");
    gtk_frame_set_child(GTK_FRAME(frame), picture);

    gtk_list_item_set_child(item, frame);
}

/* Bundles the two things on_thumb_result needs: the item (reffed, to survive
 * the hop to a worker thread and back) and app (borrowed, to update the
 * failure counter). ThumbCallback has one user_data slot, so both travel in
 * this heap struct rather than one being smuggled onto the item. */
typedef struct {
    GtkListItem* item;
    PhigApp* app;
} ThumbResultData;

/* Updates the header-bar failure counter and shows it once nonzero. Left
 * visible for the rest of the session once shown: a count dropping back out
 * of sight would look like the problem went away, which it has not — the
 * broken cell stays broken until the app restarts. */
static void phig_app_note_thumb_failed(PhigApp* app) {
    app->thumb_failed_count++;
    char* text = g_strdup_printf("%u thumbnail%s failed", app->thumb_failed_count,
                                  app->thumb_failed_count == 1 ? "" : "s");
    gtk_label_set_text(app->failed_label, text);
    g_free(text);
    gtk_widget_set_visible(GTK_WIDGET(app->failed_label), TRUE);
}

/* Delivered on the main thread by ThumbLoader. Guarded on the cancellable
 * stored on the item, not on the result: if the cell has since been unbound
 * (and its cancellable cancelled), a stale success for a now-different row
 * must not be shown. */
static void on_thumb_result(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    ThumbResultData* data = user_data;
    GtkListItem* item = data->item;
    PhigApp* app = data->app;
    g_free(data);

    GCancellable* cancellable = g_object_get_data(G_OBJECT(item), "phig-cancellable");
    if (cancellable == NULL || g_cancellable_is_cancelled(cancellable)) {
        g_object_unref(item);
        return;
    }

    GtkWidget* frame = gtk_list_item_get_child(item);
    GtkWidget* picture = gtk_frame_get_child(GTK_FRAME(frame));

    if (result == THUMB_OK) {
        gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
        gtk_widget_remove_css_class(picture, "phig-thumb-placeholder");
        gtk_widget_remove_css_class(picture, "phig-thumb-failed");
    } else if (result == THUMB_FAILED) {
        gtk_picture_set_paintable(GTK_PICTURE(picture), NULL);
        gtk_widget_add_css_class(picture, "phig-thumb-failed");
        phig_app_note_thumb_failed(app);
    } else { /* THUMB_MISS: not an error, just not generated (or generating) yet */
        gtk_picture_set_paintable(GTK_PICTURE(picture), NULL);
        gtk_widget_add_css_class(picture, "phig-thumb-placeholder");
    }

    g_object_unref(item);
}

/* bind: a placeholder row (page not fetched yet) gets the grey state and
 * nothing else. A real row gets a cancellable stashed on the item and a
 * thumbnail request; the item is reffed for the callback's user_data because
 * it must survive the hop to a worker thread and back regardless of whether
 * GTK still wants it bound when the result arrives. */
static void on_factory_bind(GtkSignalListItemFactory* factory, GtkListItem* item,
                             gpointer user_data) {
    (void)factory;
    PhigApp* app = user_data;
    PhigImageRow* row = gtk_list_item_get_item(item);

    GtkWidget* frame = gtk_list_item_get_child(item);
    GtkWidget* picture = gtk_frame_get_child(GTK_FRAME(frame));

    if (phig_image_row_is_placeholder(row)) {
        gtk_picture_set_paintable(GTK_PICTURE(picture), NULL);
        gtk_widget_add_css_class(picture, "phig-thumb-placeholder");
        gtk_widget_remove_css_class(picture, "phig-thumb-failed");
        return;
    }

    GCancellable* cancellable = g_cancellable_new();
    g_object_set_data_full(G_OBJECT(item), "phig-cancellable", cancellable, g_object_unref);

    ThumbResultData* data = g_new0(ThumbResultData, 1);
    data->item = g_object_ref(item);
    data->app = app;

    thumb_loader_request(app->loader, phig_image_row_get_id(row), phig_image_row_get_sha256(row),
                          PHIG_CELL_SIZE, cancellable, on_thumb_result, data);
}

/* unbind: cancel any outstanding request for this cell and clear the
 * picture, so a cell recycled onto a different row never flashes the
 * previous one's thumbnail while its own request is in flight. */
static void on_factory_unbind(GtkSignalListItemFactory* factory, GtkListItem* item,
                               gpointer user_data) {
    (void)factory;
    (void)user_data;
    GCancellable* cancellable = g_object_get_data(G_OBJECT(item), "phig-cancellable");
    if (cancellable != NULL)
        g_cancellable_cancel(cancellable);
    g_object_set_data(G_OBJECT(item), "phig-cancellable", NULL);

    GtkWidget* frame = gtk_list_item_get_child(item);
    GtkWidget* picture = gtk_frame_get_child(GTK_FRAME(frame));
    gtk_picture_set_paintable(GTK_PICTURE(picture), NULL);
}

/* Activated when a cell is clicked. Launches the image in the system viewer. */
static void on_cell_activate(GtkGridView* grid, guint position, gpointer user_data) {
    (void)grid;
    PhigApp* app = user_data;

    GtkSelectionModel* selection = gtk_grid_view_get_model(app->grid);
    GListModel* list_model = NULL;
    g_object_get(selection, "model", &list_model, NULL);

    if (list_model == NULL)
        return;

    PhigImageRow* row = PHIG_IMAGE_ROW(g_list_model_get_item(list_model, position));

    if (row == NULL) {
        g_object_unref(list_model);
        return;
    }

    if (phig_image_row_is_placeholder(row)) {
        g_object_unref(row);
        g_object_unref(list_model);
        return;
    }

    const char* path = phig_image_row_get_path(row);
    GFile* file = g_file_new_for_path(path);
    GtkFileLauncher* launcher = gtk_file_launcher_new(file);
    gtk_file_launcher_launch(launcher, GTK_WINDOW(app->window), NULL, on_launched, app);
    g_object_unref(file);

    g_object_unref(row);
    g_object_unref(list_model);
}

static void startup_result_free(StartupResult* result) {
    /* On success, query has been handed to the model by startup_done and
     * must not be freed here. On failure it was never allocated (query is
     * the last thing startup_thread_func sets before returning success), so
     * this only ever runs on an already-NULL query in that case. */
    g_free(result);
}

/* One toast for the whole session, not one per failed page: a scroll that
 * outruns several page fetches would otherwise stack a toast per failure.
 * The affected cells already keep their placeholders (the model's contract
 * for this signal), so this is purely a "something is wrong" notice. */
static void on_page_fetch_failed(PhigGalleryModel* model, gpointer user_data) {
    (void)model;
    PhigApp* app = user_data;
    if (app->page_fetch_toast_shown)
        return;
    app->page_fetch_toast_shown = TRUE;

    AdwToast* toast = adw_toast_new("Some images could not be loaded");
    adw_toast_overlay_add_toast(app->toast_overlay, toast);
}

/* Replaces the grid with an in-window error page carrying phig's own message.
 * The window and its header bar stay up; only the content area changes, so
 * this is never a dialog and never blocks anything. */
static void show_startup_error(PhigApp* app, const char* message) {
    GtkWidget* status_page = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(status_page), "dialog-error-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(status_page), "Could Not Open Collection");
    adw_status_page_set_description(ADW_STATUS_PAGE(status_page), message);
    adw_toolbar_view_set_content(app->toolbar_view, status_page);
}

static void startup_done(GObject* source, GAsyncResult* result, gpointer user_data) {
    (void)source;
    (void)user_data;
    GTask* task = G_TASK(result);
    StartupResult* startup = g_task_get_task_data(task);
    PhigApp* app = startup->app;

    GError* error = NULL;
    if (!g_task_propagate_boolean(task, &error)) {
        show_startup_error(app, error->message);
        g_error_free(error);
        if (startup->ctx != NULL)
            phig_shutdown(startup->ctx);
        return;
    }

    app->ctx = startup->ctx;
    app->loader = thumb_loader_new(app->ctx, PHIG_THUMB_LOADER_THREADS, PHIG_THUMB_CACHE_BYTES);

    /* The model borrows ctx/query for its lifetime. Both outlive it here:
     * ctx lives on app until phig_app_free at process exit, and query is
     * never freed once handed to the model (phig-gtk has no query-replace
     * path yet, so the model is never torn down before the app is). */
    PhigGalleryModel* model = phig_gallery_model_new(app->ctx, startup->query);
    g_signal_connect(model, "page-fetch-failed", G_CALLBACK(on_page_fetch_failed), app);
    GtkSelectionModel* selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(model)));
    gtk_grid_view_set_model(app->grid, selection);
    g_object_unref(selection);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), app);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);
    gtk_grid_view_set_factory(app->grid, factory);
    g_object_unref(factory);
}

static void on_activate(GtkApplication* gtk_app, gpointer user_data) {
    PhigApp* app = user_data;

    app->window = ADW_APPLICATION_WINDOW(adw_application_window_new(gtk_app));
    gtk_window_set_title(GTK_WINDOW(app->window), "phig");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 750);

    app->toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar* header_bar = ADW_HEADER_BAR(adw_header_bar_new());

    /* Unobtrusive running count of THUMB_FAILED results (Step 3). Hidden
     * until the first failure so a normal run — all hits and misses — shows
     * nothing extra in the header bar. */
    app->failed_label = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(app->failed_label), "error");
    gtk_widget_set_visible(GTK_WIDGET(app->failed_label), FALSE);
    adw_header_bar_pack_end(header_bar, GTK_WIDGET(app->failed_label));
    adw_toolbar_view_add_top_bar(app->toolbar_view, GTK_WIDGET(header_bar));

    app->grid = GTK_GRID_VIEW(gtk_grid_view_new(NULL, NULL));
    gtk_grid_view_set_min_columns(app->grid, 3);
    g_signal_connect(app->grid, "activate", G_CALLBACK(on_cell_activate), app);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(app->grid));
    adw_toolbar_view_set_content(app->toolbar_view, scroller);

    app->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(app->toast_overlay, GTK_WIDGET(app->toolbar_view));

    adw_application_window_set_content(app->window, GTK_WIDGET(app->toast_overlay));

    /* The window maps with an empty grid; the query runs on a worker and the
     * model is not attached until it finishes (startup_done, above). */
    gtk_window_present(GTK_WINDOW(app->window));

    StartupResult* startup = g_new0(StartupResult, 1);
    startup->app = app;
    GTask* task = g_task_new(NULL, NULL, startup_done, NULL);
    g_task_set_task_data(task, startup, (GDestroyNotify)startup_result_free);
    g_task_run_in_thread(task, startup_thread_func);
    g_object_unref(task);
}

/* $XDG_DATA_HOME/phig/phig.db, or ~/.local/share/phig/phig.db — phig's own
 * default (see phig/src/paths.cpp), duplicated here because phig_config.db_path
 * is required and libphig exposes no accessor for its default. */
static char* default_db_path(void) {
    const char* xdg = g_getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0')
        return g_build_filename(xdg, "phig", "phig.db", NULL);
    return g_build_filename(g_get_home_dir(), ".local", "share", "phig", "phig.db", NULL);
}

int main(int argc, char** argv) {
    PhigApp* app = g_new0(PhigApp, 1);
    app->db_path = (argc > 1) ? g_strdup(argv[1]) : default_db_path();

    /* argv is parsed above, not handed to GApplication: G_APPLICATION_DEFAULT_FLAGS
     * treats a bare positional argument as a file to open, which a db path is not. */
    AdwApplication* gtk_app = adw_application_new("org.phig.Gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), app);
    int status = g_application_run(G_APPLICATION(gtk_app), 1, argv);
    g_object_unref(gtk_app);
    phig_app_free(app);
    return status;
}
