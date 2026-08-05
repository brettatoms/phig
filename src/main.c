#include <adwaita.h>
#include <gtk/gtk.h>
#include <phig/phig.h>
#include <stdlib.h>

#include "gallery-model.h"
#include "thumb-loader.h"

/* Cell size in the grid. */
#define PHIG_CELL_SIZE 200

/* Loader thread count: measured at 2, 4, and 8 against warm and miss-heavy
 * full-range scrolls (task-7-report.md). Frame timing is identical across all
 * three on a warm cache (main-thread texture/placeholder work dominates, not
 * worker count) and on a miss-heavy scroll (capped by HDD seek time
 * regardless of thread count). 8 threads also reproduced a pre-existing,
 * unrelated crash — concurrent thumbnail-generation attempts against one
 * dangling DB row whose source file no longer exists on disk — that did not
 * reproduce at 4. 4 is kept: no measured benefit from more, and it avoids
 * that crash. */
#define PHIG_THUMB_LOADER_THREADS 4

/* Texture cache budget: measured at 64/256/512 MB against the full
 * 29,953-row warm scroll (task-7-report.md). Frame timing did not
 * distinguish the three (66-83ms worst frame at all of them); RSS scaled
 * with the budget (276/476/692 MB peak). 256 MB is kept over 64 MB for
 * scrollback headroom — a monotonic one-way sweep never revisits a texture,
 * so this test cannot show the difference a back-and-forth scroll would. */
#define PHIG_THUMB_CACHE_BYTES (256 * 1024 * 1024)

/* Every tunable is also overridable via environment variable so the same
 * binary can be re-measured at different values without a rebuild
 * (task-7-report.md's Step 4 sweep). Falls back to the #define when unset or
 * unparseable. */
static guint env_override_uint(const char* name, guint fallback) {
    const char* value = g_getenv(name);
    if (value == NULL || value[0] == '\0')
        return fallback;
    char* end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || parsed <= 0)
        return fallback;
    return (guint)parsed;
}

typedef struct {
    AdwApplicationWindow* window;
    AdwToolbarView* toolbar_view;
    GtkGridView* grid;
    GtkScrolledWindow* scroller;
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

    gint64 startup_start_us; /* g_get_monotonic_time() at process start, for PHIG_MEASURE */
} PhigApp;

static void phig_app_free(PhigApp* app) {
    g_clear_object(&app->loader);
    if (app->ctx != NULL)
        phig_shutdown(app->ctx);
    g_free(app->db_path);
    g_free(app);
}

/* Measurement harness (task-7-report.md). Entirely gated on PHIG_MEASURE=1,
 * so it never runs in normal use: no input-injection path exists on this
 * machine (no ydotool/wtype/AT-SPI — established in Tasks 4-5), so scrolling
 * is driven directly on the scrolled window's GtkAdjustment instead, from a
 * g_timeout_add step function, with frame timings recorded via a tick
 * callback on the grid. */
typedef struct {
    PhigApp* app;
    GArray* frame_deltas_us; /* gint64, one per observed frame */
    gint64 last_frame_time_us;
    gboolean have_last_frame;
    guint tick_id;
    guint timeout_id;
    double step; /* adjustment units per timeout tick */
    gboolean returning; /* sweeping back down after reaching upper bound */
} PhigMeasureState;

static void phig_measure_state_free(PhigMeasureState* state) {
    g_array_free(state->frame_deltas_us, TRUE);
    g_free(state);
}

static gboolean phig_measure_tick(GtkWidget* widget, GdkFrameClock* clock, gpointer user_data) {
    (void)widget;
    PhigMeasureState* state = user_data;
    gint64 now_us = gdk_frame_clock_get_frame_time(clock);
    if (state->have_last_frame) {
        gint64 delta = now_us - state->last_frame_time_us;
        g_array_append_val(state->frame_deltas_us, delta);
        if (g_getenv("PHIG_MEASURE_DEBUG") != NULL)
            g_printerr("PHIG_MEASURE_DEBUG tick delta_ms=%.3f\n", delta / 1000.0);
    }
    state->last_frame_time_us = now_us;
    state->have_last_frame = TRUE;
    return G_SOURCE_CONTINUE;
}

static void phig_measure_report(PhigMeasureState* state) {
    GArray* deltas = state->frame_deltas_us;
    gint64 worst_us = 0;
    guint over_16ms = 0;
    for (guint i = 0; i < deltas->len; i++) {
        gint64 d = g_array_index(deltas, gint64, i);
        if (d > worst_us)
            worst_us = d;
        if (d > 16000)
            over_16ms++;
    }
    g_printerr("PHIG_MEASURE scroll frames=%u worst_frame_ms=%.3f over_16ms=%u\n", deltas->len,
               worst_us / 1000.0, over_16ms);
}

/* Advances the vertical adjustment one step per timeout firing: sweeps from
 * lower to upper, then back to lower, then stops and reports. Two passes
 * (down and back up) exercise both scroll directions over the same range in
 * one run. */
static gboolean phig_measure_step(gpointer user_data) {
    PhigMeasureState* state = user_data;
    GtkAdjustment* vadj = gtk_scrolled_window_get_vadjustment(state->app->scroller);
    double value = gtk_adjustment_get_value(vadj);
    double lower = gtk_adjustment_get_lower(vadj);
    double upper = gtk_adjustment_get_upper(vadj) - gtk_adjustment_get_page_size(vadj);
    if (g_getenv("PHIG_MEASURE_DEBUG") != NULL)
        g_printerr("PHIG_MEASURE_DEBUG value=%.1f lower=%.1f upper=%.1f page_size=%.1f\n", value,
                   lower, upper, gtk_adjustment_get_page_size(vadj));

    /* The grid has no layout yet on the first few ticks after model attach
     * (page_size/upper read 0 until GTK completes a size allocation), which
     * would otherwise read as "already at the end" and finish the sweep
     * before it starts. Wait for a real page size instead of stepping. */
    if (gtk_adjustment_get_page_size(vadj) <= 0.0)
        return G_SOURCE_CONTINUE;

    if (!state->returning) {
        value += state->step;
        if (value >= upper) {
            value = upper;
            state->returning = TRUE;
        }
    } else {
        value -= state->step;
        if (value <= lower) {
            value = lower;
            gtk_adjustment_set_value(vadj, value);
            phig_measure_report(state);
            gtk_widget_remove_tick_callback(GTK_WIDGET(state->app->grid), state->tick_id);
            phig_measure_state_free(state);
            g_application_quit(g_application_get_default());
            return G_SOURCE_REMOVE;
        }
    }
    gtk_adjustment_set_value(vadj, value);
    return G_SOURCE_CONTINUE;
}

/* Starts the scroll harness once PHIG_MEASURE=1 is set and the model is
 * attached. PHIG_MEASURE_STEP_PX (default 4000) controls how many adjustment
 * units advance per 8ms timeout tick — the full 29,953-row grid is roughly
 * 2,000,000px tall at 3 columns and PHIG_CELL_SIZE=200, so the default sweeps
 * end to end in a few seconds. */
static void phig_measure_maybe_start(PhigApp* app) {
    if (g_getenv("PHIG_MEASURE") == NULL)
        return;

    PhigMeasureState* state = g_new0(PhigMeasureState, 1);
    state->app = app;
    state->frame_deltas_us = g_array_new(FALSE, FALSE, sizeof(gint64));
    state->step = (double)env_override_uint("PHIG_MEASURE_STEP_PX", 4000);
    state->tick_id =
        gtk_widget_add_tick_callback(GTK_WIDGET(app->grid), phig_measure_tick, state, NULL);
    state->timeout_id = g_timeout_add(8, phig_measure_step, state);
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
    guint n_threads = env_override_uint("PHIG_THUMB_LOADER_THREADS", PHIG_THUMB_LOADER_THREADS);
    gsize cache_bytes =
        (gsize)env_override_uint("PHIG_THUMB_CACHE_BYTES", PHIG_THUMB_CACHE_BYTES);
    app->loader = thumb_loader_new(app->ctx, n_threads, cache_bytes);

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

    /* First-page-applied: the model now has a real GListModel attached and
     * GTK will query it on the next layout pass. This is the end of the
     * window announced under PHIG_MEASURE=1 (task-7-report.md's startup
     * table); nothing here waits for that layout pass to actually run, since
     * the model attach itself is the last thing this codebase controls. */
    if (g_getenv("PHIG_MEASURE") != NULL) {
        gint64 elapsed_us = g_get_monotonic_time() - app->startup_start_us;
        g_printerr("PHIG_MEASURE startup_ms=%.3f\n", elapsed_us / 1000.0);
    }

    phig_measure_maybe_start(app);
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
    app->scroller = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(app->scroller, GTK_WIDGET(app->grid));
    adw_toolbar_view_set_content(app->toolbar_view, GTK_WIDGET(app->scroller));

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
    app->startup_start_us = g_get_monotonic_time();
    app->db_path = (argc > 1) ? g_strdup(argv[1]) : default_db_path();

    /* argv is parsed above, not handed to GApplication: G_APPLICATION_DEFAULT_FLAGS
     * treats a bare positional argument as a file to open, which a db path is not. */
    AdwApplication* gtk_app = adw_application_new("org.phig.Gtk", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), app);
    int status = g_application_run(G_APPLICATION(gtk_app), 1, argv);

    /* g_application_run stops pumping the main context the moment it
     * returns. thumb_loader_dispose (see its comment) deliberately never
     * pumps the context itself to wait for outstanding idle-source
     * deliveries to drop their ref on the loader; it relies on the app's own
     * main loop still running for that. With the loop stopped, any request
     * whose result was posted but not yet delivered holds a ref that would
     * otherwise never drop, so g_clear_object below would leave the loader's
     * refcount above zero, its dispose (and the g_thread_join inside it)
     * would never run, and a worker could still be executing inside
     * libphig.so when exit() starts tearing down shared libraries — observed
     * as a SIGSEGV on quit under scroll load (task-7-report.md). Draining the
     * context here runs every pending idle delivery first, which drops their
     * refs and lets phig_app_free's g_clear_object bring the loader to
     * dispose synchronously, joining every worker before this function
     * returns. */
    while (app->loader != NULL && thumb_loader_pending_idle_count(app->loader) > 0)
        g_main_context_iteration(NULL, TRUE);

    g_object_unref(gtk_app);
    phig_app_free(app);
    return status;
}
