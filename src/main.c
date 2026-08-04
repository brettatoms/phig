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
    GtkGridView* grid;
    char* db_path;

    /* Owned once startup finishes; NULL until then. Kept on the app struct so
     * shutdown can free them in order (loader before ctx: workers hold ctx
     * for as long as they run). */
    phig_ctx* ctx;
    ThumbLoader* loader;
} PhigApp;

static void phig_app_free(PhigApp* app) {
    g_clear_object(&app->loader);
    if (app->ctx != NULL)
        phig_shutdown(app->ctx);
    g_free(app->db_path);
    g_free(app);
}

/* Result of the worker startup sequence, handed to the main thread. On
 * failure ctx/query are NULL and error explains why. */
typedef struct {
    PhigApp* app; /* borrowed; app outlives this, freed only at process exit */
    phig_ctx* ctx;
    phig_query* query;
} StartupResult;

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
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "phig_init failed: %d",
                                 (int)status);
        return;
    }

    status = phig_query_new(result->ctx, &result->query);
    if (status != PHIG_OK) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "phig_query_new failed: %d",
                                 (int)status);
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

/* Delivered on the main thread by ThumbLoader. user_data is the GtkListItem,
 * reffed when the request was made since the loader holds it across a hop to
 * a worker thread and back. Guarded on the cancellable stored on the item,
 * not on the result: if the cell has since been unbound (and its cancellable
 * cancelled), a stale success for a now-different row must not be shown. */
static void on_thumb_result(ThumbResult result, GdkTexture* texture, gpointer user_data) {
    GtkListItem* item = user_data;
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
    } else { /* THUMB_MISS */
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
    ThumbLoader* loader = user_data;
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

    thumb_loader_request(loader, phig_image_row_get_sha256(row), PHIG_CELL_SIZE, cancellable,
                          on_thumb_result, g_object_ref(item));
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

static void startup_result_free(StartupResult* result) {
    /* On success, query has been handed to the model by startup_done and
     * must not be freed here. On failure it was never allocated (query is
     * the last thing startup_thread_func sets before returning success), so
     * this only ever runs on an already-NULL query in that case. */
    g_free(result);
}

static void startup_done(GObject* source, GAsyncResult* result, gpointer user_data) {
    (void)source;
    (void)user_data;
    GTask* task = G_TASK(result);
    StartupResult* startup = g_task_get_task_data(task);
    PhigApp* app = startup->app;

    GError* error = NULL;
    if (!g_task_propagate_boolean(task, &error)) {
        g_warning("phig-gtk: startup failed: %s", error->message);
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
    GtkSelectionModel* selection = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(model)));
    gtk_grid_view_set_model(app->grid, selection);
    g_object_unref(selection);

    GtkListItemFactory* factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_factory_setup), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_factory_bind), app->loader);
    g_signal_connect(factory, "unbind", G_CALLBACK(on_factory_unbind), NULL);
    gtk_grid_view_set_factory(app->grid, factory);
    g_object_unref(factory);
}

static void on_activate(GtkApplication* gtk_app, gpointer user_data) {
    PhigApp* app = user_data;

    app->window = ADW_APPLICATION_WINDOW(adw_application_window_new(gtk_app));
    gtk_window_set_title(GTK_WINDOW(app->window), "phig");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 750);

    AdwToolbarView* view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, adw_header_bar_new());

    app->grid = GTK_GRID_VIEW(gtk_grid_view_new(NULL, NULL));
    gtk_grid_view_set_min_columns(app->grid, 3);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), GTK_WIDGET(app->grid));
    adw_toolbar_view_set_content(view, scroller);

    adw_application_window_set_content(app->window, GTK_WIDGET(view));

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
