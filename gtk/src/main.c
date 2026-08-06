#include <adwaita.h>
#include <gtk/gtk.h>
#include <math.h>
#include <phig/phig.h>
#include <stdlib.h>

#include "gallery-model.h"
#include "thumb-loader.h"

/* Grid zoom. The grid always divides the viewport width exactly — cell size is
 * derived as viewport_width / columns, never set directly — so there is no
 * horizontal scrolling and no ragged right edge at any zoom level or window
 * size. Zoom picks the column count; resizing keeps the count and changes the
 * cell size.
 *
 * Zoom sets a target cell size, and the column count is derived from it and the
 * viewport width. Resizing the window therefore reflows the columns and keeps
 * thumbnails about the size the user picked, rather than stretching them. The
 * derived cell size is then whatever divides the width exactly, so it lands
 * within half a column of the target.
 *
 * The bounds stop the grid reflowing to unusably small cells on a wide monitor
 * (max) or to cells the thumbnail cache cannot serve sharply (min is
 * additionally clamped at runtime — see phig_min_columns_for_width). */
#define PHIG_CELL_TARGET_PX 200

/* Zoom range, as logical cell sizes. The steps are multiplicative so each zoom
 * feels like the same amount of change at every size. The top of the range is
 * bounded by what the thumbnail cache can serve sharply at the current scale
 * factor (see phig_min_columns_for_width), not by PHIG_CELL_MAX_PX alone. */
#define PHIG_CELL_MIN_PX 80
#define PHIG_CELL_MAX_PX 480
#define PHIG_ZOOM_STEP 1.25

#define PHIG_COLUMNS_MIN 1
#define PHIG_COLUMNS_MAX 24

/* Largest thumbnail the phig cache holds (longest edge, core/thumbs.cpp
 * THUMB_SIZE). A cell needing more physical pixels than this would be upscaled
 * and look soft, which is what bounds zooming in. */
#define PHIG_CACHE_THUMB_PX 512

/* Requested texture sizes are rounded up to a multiple of this, so a 187px and
 * a 195px cell share one cached texture instead of each getting their own. Keeps
 * the (sha256, size) cache from growing a generation per pixel and means an
 * ordinary window resize usually does not invalidate anything. */
#define PHIG_TEXTURE_SIZE_QUANTUM 64

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

    /* Zoom state. `columns` is the resting column count; `pinch_columns` tracks
     * the fractional count during a live pinch so the snap decision can see
     * which direction the gesture is going. */
    guint target_cell_px;    /* what zoom sets; held constant across window resizes */
    guint columns;           /* derived from target_cell_px and the viewport width */
    guint relayout_id;       /* pending phig_relayout_idle, 0 if none */
    gboolean relayout_needs_rebind; /* geometry changed: re-request textures at the new size */
    double pinch_cell;      /* live target size during a pinch */
    guint pinch_base_cell;  /* target size when the gesture began */
} PhigApp;

/* Shutdown order matters: loader workers must stop touching ctx before ctx is
 * torn down, and the model's own in-flight page fetches (which also run on
 * ctx, via a worker thread) must finish before phig_shutdown as well.
 * thumb_loader_shutdown joins every worker; dropping the grid's model then
 * drops the model's last ref, running phig_gallery_model_dispose (which waits
 * on its in_flight counter) synchronously before this function continues. */
static void phig_app_free(PhigApp* app) {
    if (app->relayout_id != 0) {
        g_source_remove(app->relayout_id);
        app->relayout_id = 0;
    }
    if (app->loader != NULL)
        thumb_loader_shutdown(app->loader);
    g_clear_object(&app->loader);

    if (app->grid != NULL)
        gtk_grid_view_set_model(app->grid, NULL);

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
    gint64 refresh_interval_us; /* from the frame clock; 0 until first sampled */
} PhigMeasureState;

static void phig_measure_state_free(PhigMeasureState* state) {
    g_array_free(state->frame_deltas_us, TRUE);
    g_free(state);
}

static gboolean phig_measure_tick(GtkWidget* widget, GdkFrameClock* clock, gpointer user_data) {
    (void)widget;
    PhigMeasureState* state = user_data;
    gint64 now_us = gdk_frame_clock_get_frame_time(clock);
    if (state->refresh_interval_us == 0) {
        gint64 interval = 0, presentation = 0;
        gdk_frame_clock_get_refresh_info(clock, now_us, &interval, &presentation);
        state->refresh_interval_us = interval;
    }
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

static int phig_measure_cmp_us(const void* a, const void* b) {
    gint64 x = *(const gint64*)a, y = *(const gint64*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* A frame is only late once it misses a whole refresh interval: a correctly
 * delivered 60 Hz frame is 16.667 ms, so counting frames over a flat 16.000 ms
 * marks every healthy frame as a failure. That bug is what produced M0's
 * "95% of frames over 16 ms" — the real figure at the time was ~5% dropped.
 * The interval comes from the frame clock, so a 120 Hz or 144 Hz display
 * measures against its own budget rather than a hardcoded 60 Hz.
 *
 * Percentiles are reported alongside the count because a single ratio hid that
 * bug for a whole milestone: a p50 sitting exactly on the refresh interval is
 * the tell that the grid is healthy and the threshold is wrong. */
static void phig_measure_report(PhigMeasureState* state) {
    GArray* deltas = state->frame_deltas_us;
    if (deltas->len == 0) {
        g_printerr("PHIG_MEASURE scroll frames=0\n");
        return;
    }

    gint64 interval_us = state->refresh_interval_us > 0 ? state->refresh_interval_us : 16667;

    /* Late = missed the next vsync deadline. A small tolerance keeps jitter in
     * the timestamp itself from reading as a drop. */
    const gint64 tolerance_us = interval_us / 10;
    guint late = 0;
    for (guint i = 0; i < deltas->len; i++) {
        if (g_array_index(deltas, gint64, i) > interval_us + tolerance_us)
            late++;
    }

    GArray* sorted = g_array_sized_new(FALSE, FALSE, sizeof(gint64), deltas->len);
    g_array_append_vals(sorted, deltas->data, deltas->len);
    qsort(sorted->data, sorted->len, sizeof(gint64), phig_measure_cmp_us);
    gint64 p50 = g_array_index(sorted, gint64, sorted->len / 2);
    gint64 p90 = g_array_index(sorted, gint64, (guint)(sorted->len * 0.90));
    gint64 p99 = g_array_index(sorted, gint64, (guint)(sorted->len * 0.99));
    gint64 worst = g_array_index(sorted, gint64, sorted->len - 1);
    g_array_free(sorted, TRUE);

    g_printerr("PHIG_MEASURE scroll frames=%u refresh_ms=%.3f p50_ms=%.3f p90_ms=%.3f "
               "p99_ms=%.3f worst_ms=%.3f late=%u (%.1f%%)\n",
               deltas->len, interval_us / 1000.0, p50 / 1000.0, p90 / 1000.0, p99 / 1000.0,
               worst / 1000.0, late, 100.0 * late / deltas->len);
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
 * 2,000,000px tall at the default column count, so the default sweeps
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


/* ---- Grid zoom geometry ----
 *
 * Cell size is always derived from the viewport width so the row exactly fills
 * it. GtkGridView lays out `columns` equal columns; each cell requests
 * width/columns, so there is no leftover width to show as a ragged edge. */

static int phig_viewport_width(PhigApp* app) {
    int w = gtk_widget_get_width(GTK_WIDGET(app->scroller));
    if (w > 0)
        return w;
    /* Before the first allocation the widget reports 0; the hadjustment's page
     * size is the same visible width and is set slightly earlier. */
    GtkAdjustment* hadj = gtk_scrolled_window_get_hadjustment(app->scroller);
    double page = hadj != NULL ? gtk_adjustment_get_page_size(hadj) : 0.0;
    return page > 0.0 ? (int)page : 0;
}

static guint phig_cell_size_for(int viewport_width, guint columns) {
    if (columns == 0)
        return 1;
    int per = viewport_width / (int)columns;
    return per > 1 ? (guint)per : 1;
}

/* Smallest column count whose derived cell still fits the thumbnail cache at
 * this scale factor. Fewer columns means bigger cells; past this point a cell
 * needs more physical pixels than the cache holds and thumbnails go soft, so
 * this is the zoom-in limit — computed from the cache size rather than
 * hardcoded. */
static guint phig_min_columns_for_width(int viewport_width, int scale) {
    if (viewport_width <= 0 || scale <= 0)
        return PHIG_COLUMNS_MIN;
    /* Want: (width / columns) * scale <= CACHE  =>  columns >= width * scale / CACHE */
    guint needed = (guint)((viewport_width * scale + PHIG_CACHE_THUMB_PX - 1) / PHIG_CACHE_THUMB_PX);
    return needed > PHIG_COLUMNS_MIN ? needed : PHIG_COLUMNS_MIN;
}

/* Column count that puts the derived cell size closest to `target_px` for this
 * viewport width. This is the whole resize policy: the target is held and the
 * count moves, so thumbnails keep their size and the grid reflows instead. */
static guint phig_columns_for_target(int viewport_width, guint target_px) {
    if (viewport_width <= 0 || target_px == 0)
        return 1; /* clamped once a real width arrives */
    guint columns = (guint)(((guint)viewport_width + target_px / 2) / target_px);
    return columns > 0 ? columns : 1;
}

static guint phig_clamp_columns(PhigApp* app, guint columns) {
    int width = phig_viewport_width(app);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(app->grid));
    if (scale < 1)
        scale = 1;
    guint lo = phig_min_columns_for_width(width, scale);
    guint hi = PHIG_COLUMNS_MAX;
    if (lo > hi)
        lo = hi;
    if (columns < lo)
        return lo;
    if (columns > hi)
        return hi;
    return columns;
}

/* Physical pixels a cell needs, rounded up to the quantum so nearby cell sizes
 * share one cached texture. */
static guint phig_texture_size_for(PhigApp* app) {
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(app->grid));
    if (scale < 1)
        scale = 1;
    guint physical = phig_cell_size_for(phig_viewport_width(app), app->columns) * (guint)scale;
    guint quantised = ((physical + PHIG_TEXTURE_SIZE_QUANTUM - 1) / PHIG_TEXTURE_SIZE_QUANTUM) *
                      PHIG_TEXTURE_SIZE_QUANTUM;
    /* Never ask for more than the cache can supply — it would be upscaled. */
    return quantised > PHIG_CACHE_THUMB_PX ? PHIG_CACHE_THUMB_PX : quantised;
}

/* Applies the current column count: tells GtkGridView how many columns to lay
 * out and re-sizes the visible cells. Cells re-request their size on the next
 * layout pass, and each rebind picks up the new texture size. */
static void phig_apply_columns(PhigApp* app) {
    /* Setting these properties already queues a resize; calling
     * queue_resize as well re-enters layout from inside layout, which GTK
     * reports as "layout continuously requested, giving up after 4 tries". */
    gtk_grid_view_set_min_columns(app->grid, app->columns);
    gtk_grid_view_set_max_columns(app->grid, app->columns);
}

static void phig_queue_relayout(PhigApp* app);

/* Zoom entry point: sets the size thumbnails should be, then re-derives the
 * column count. Everything else (resize, scale change) goes through
 * phig_queue_relayout, which re-derives from the unchanged target. */
static void phig_set_target_cell(PhigApp* app, guint target_px) {
    guint lo = PHIG_CELL_MIN_PX, hi = PHIG_CELL_MAX_PX;
    if (target_px < lo)
        target_px = lo;
    if (target_px > hi)
        target_px = hi;
    if (target_px == app->target_cell_px)
        return;
    app->target_cell_px = target_px;
    phig_queue_relayout(app);
}

/* Snap a fractional column count to a whole one, with direction-dependent
 * hysteresis. Pinching toward bigger cells (fewer columns) only commits once
 * the edge column is less than 40% shown; pinching toward smaller cells only
 * commits past 60%. The 0.4-0.6 dead zone holds the current count, so a gesture
 * hovering on a boundary does not oscillate between two layouts. */
static guint phig_snap_columns(double fractional, guint current) {
    if (fractional <= 0.0)
        return current;
    double floor_part = floor(fractional);
    double frac = fractional - floor_part;
    guint lower = (guint)floor_part;
    if (lower == 0)
        return 1;

    gboolean toward_fewer = fractional < (double)current; /* fewer columns = bigger cells */
    double threshold = toward_fewer ? 0.4 : 0.6;
    return frac > threshold ? lower + 1 : lower;
}


/* Cells are NOT given a size request. GtkGridView divides its allocated width
 * among max_columns equal columns, so the cell width already follows the
 * viewport. A size request here would set each cell's MINIMUM size, and a
 * window's natural size is derived from its children's minimums — so a bigger
 * cell would demand a wider window, which would widen the viewport, which would
 * derive an even bigger cell. That loop grows the window without bound until the
 * process is killed. Only the height needs setting, to keep cells square. */
static void phig_resize_visible_cells(PhigApp* app) {
    guint cell = phig_cell_size_for(phig_viewport_width(app), app->columns);
    for (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(app->grid)); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        for (GtkWidget* inner = gtk_widget_get_first_child(child); inner != NULL;
             inner = gtk_widget_get_next_sibling(inner)) {
            if (!GTK_IS_FRAME(inner))
                continue;
            int cur_w = -1, cur_h = -1;
            gtk_widget_get_size_request(inner, &cur_w, &cur_h);
            /* Width stays -1 (unconstrained, follows the column); height makes
             * the cell square. Height cannot feed back into the width. */
            if (cur_w != -1 || cur_h != (int)cell)
                gtk_widget_set_size_request(inner, -1, (int)cell);
        }
    }
}

static gboolean phig_relayout_idle(gpointer user_data) {
    PhigApp* app = user_data;
    app->relayout_id = 0;

    int width = phig_viewport_width(app);
    if (width <= 0)
        return G_SOURCE_REMOVE;

    /* Re-derive the column count from the target cell size. A resize changes the
     * count and keeps thumbnails at their chosen size; zoom changes the target
     * and is what makes thumbnails bigger or smaller. Assigning directly rather
     * than via phig_set_columns because that clamps against the current count
     * and no-ops when the clamp agrees with it, which would strand the
     * provisional value on the first pass. */
    guint derived = phig_clamp_columns(app, phig_columns_for_target(width, app->target_cell_px));
    if (derived != app->columns) {
        app->columns = derived;
        app->relayout_needs_rebind = TRUE;
        phig_apply_columns(app);
    }
    phig_resize_visible_cells(app);
    /* Cells bound before this point requested a texture for the previous cell
     * size — at startup that is the provisional 1-column geometry, which is far
     * too large and renders visibly blurry until the cell is recycled. Dropping
     * the model and re-setting it rebinds every visible cell at the new size. */
    if (app->relayout_needs_rebind) {
        app->relayout_needs_rebind = FALSE;
        GtkSelectionModel* model = gtk_grid_view_get_model(app->grid);
        if (model != NULL) {
            g_object_ref(model);
            gtk_grid_view_set_model(app->grid, NULL);
            gtk_grid_view_set_model(app->grid, model);
            g_object_unref(model);
        }
    }
    return G_SOURCE_REMOVE;
}

static void phig_queue_relayout(PhigApp* app) {
    if (app->relayout_id == 0)
        app->relayout_id = g_idle_add(phig_relayout_idle, app);
}

static void on_grid_width_changed(GObject* object, GParamSpec* pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    phig_queue_relayout(user_data);
}

static void on_scale_factor_changed(GObject* object, GParamSpec* pspec, gpointer user_data) {
    (void)object;
    (void)pspec;
    /* Moving between monitors changes how many physical pixels a cell needs, so
     * the sharpness clamp and the requested texture size both change. */
    phig_queue_relayout(user_data);
}

/* Trackpad pinch. The gesture is continuous but the resting state is always a
 * whole column count: the scale is tracked live and snapped on release. */
static void on_zoom_begin(GtkGesture* gesture, GdkEventSequence* sequence, gpointer user_data) {
    (void)gesture;
    (void)sequence;
    PhigApp* app = user_data;
    app->pinch_base_cell = app->target_cell_px;
    app->pinch_cell = (double)app->target_cell_px;
}

static void on_zoom_scale_changed(GtkGestureZoom* gesture, double scale, gpointer user_data) {
    (void)gesture;
    PhigApp* app = user_data;
    if (scale <= 0.0)
        return;
    /* Pinching out (scale > 1) magnifies, i.e. bigger thumbnails. */
    app->pinch_cell = (double)app->pinch_base_cell * scale;
}

/* Snap on release so the resting state is a whole number of columns filling the
 * width, then adopt the cell size that count implies as the new target. Uses the
 * hysteresis rule so a pinch resting on a boundary does not oscillate. */
static void on_zoom_end(GtkGesture* gesture, GdkEventSequence* sequence, gpointer user_data) {
    (void)gesture;
    (void)sequence;
    PhigApp* app = user_data;
    int width = phig_viewport_width(app);
    if (width <= 0 || app->pinch_cell <= 0.0)
        return;
    double fractional = (double)width / app->pinch_cell;
    guint snapped = phig_clamp_columns(app, phig_snap_columns(fractional, app->columns));
    phig_set_target_cell(app, phig_cell_size_for(width, snapped));
}

/* Ctrl+scroll, for mice. Discrete by nature, so it steps one column per notch
 * and needs no snapping. */
static gboolean on_scroll(GtkEventControllerScroll* controller, double dx, double dy,
                          gpointer user_data) {
    (void)dx;
    PhigApp* app = user_data;
    GdkModifierType state =
        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
    if ((state & GDK_CONTROL_MASK) == 0)
        return GDK_EVENT_PROPAGATE; /* plain scroll must still scroll the grid */
    if (dy == 0.0)
        return GDK_EVENT_STOP;
    /* Scrolling up (dy < 0) zooms in: bigger thumbnails. */
    double next = dy < 0.0 ? app->target_cell_px * PHIG_ZOOM_STEP
                           : app->target_cell_px / PHIG_ZOOM_STEP;
    phig_set_target_cell(app, (guint)(next + 0.5));
    return GDK_EVENT_STOP;
}

/* setup: build the cell. content-fit COVER keeps thumbnails from distorting;
 * the frame gives every cell the same footprint whether or not it currently has
 * a picture. Size is applied at bind time, not here — see on_factory_setup. */
static void on_factory_setup(GtkSignalListItemFactory* factory, GtkListItem* item,
                              gpointer user_data) {
    (void)factory;
    (void)user_data;
    GtkWidget* frame = gtk_frame_new(NULL);
    /* No size request here: the cell's footprint depends on the viewport width
     * and the current column count, neither of which setup can see. on_factory_bind
     * sets it, and phig_resize_visible_cells updates it on resize/zoom. */

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

    /* Before the placeholder early-return: a placeholder cell still needs a
     * row's worth of height, or rows collapse while pages are in flight. Height
     * only — see phig_resize_visible_cells for why width must stay
     * unconstrained. */
    guint cell = phig_cell_size_for(phig_viewport_width(app), app->columns);
    gtk_widget_set_size_request(frame, -1, (int)cell);

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
                          phig_texture_size_for(app), cancellable, on_thumb_result, data);
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
    /* query is never freed here. On success it is borrowed by the model for
     * the model's lifetime (phig-gtk has no query-replace path yet, so that
     * lifetime is the rest of the process). On failure, startup_done frees it
     * itself before returning, since phig_query_new succeeding followed by a
     * later fallible step (currently just phig_query_set_sort, but nothing
     * stops that changing) is the one path where query is allocated but never
     * reaches the model. */
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
        if (startup->query != NULL)
            phig_query_free(startup->query);
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
    /* Provisional until the first width notification; phig_default_columns_for_width
     * cannot run before the scroller has been allocated. */
    app->columns = 1; /* provisional until the first real viewport width */
    app->target_cell_px = PHIG_CELL_TARGET_PX;
    /* min == max pins the column count: the grid must lay out exactly the count
     * zoom chose, so cells divide the width instead of GridView picking its own
     * count from the cells' requested size. */
    gtk_grid_view_set_min_columns(app->grid, app->columns);
    gtk_grid_view_set_max_columns(app->grid, app->columns);
    g_signal_connect(app->grid, "activate", G_CALLBACK(on_cell_activate), app);
    app->scroller = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_child(app->scroller, GTK_WIDGET(app->grid));
    /* Never scroll horizontally: cell size is derived from the viewport width,
     * so a horizontal scrollbar would mean the derivation is wrong. */
    gtk_scrolled_window_set_policy(app->scroller, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    /* GtkWidget exposes no resize signal and no notifying width property, so the
     * horizontal adjustment's page size — which is the visible width — is what
     * reports a viewport resize. */
    g_signal_connect(gtk_scrolled_window_get_hadjustment(app->scroller), "notify::page-size",
                     G_CALLBACK(on_grid_width_changed), app);
    g_signal_connect(app->grid, "notify::scale-factor", G_CALLBACK(on_scale_factor_changed), app);

    GtkGesture* zoom = gtk_gesture_zoom_new();
    g_signal_connect(zoom, "begin", G_CALLBACK(on_zoom_begin), app);
    g_signal_connect(zoom, "scale-changed", G_CALLBACK(on_zoom_scale_changed), app);
    g_signal_connect(zoom, "end", G_CALLBACK(on_zoom_end), app);
    gtk_widget_add_controller(GTK_WIDGET(app->grid), GTK_EVENT_CONTROLLER(zoom));

    GtkEventController* scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), app);
    gtk_widget_add_controller(GTK_WIDGET(app->grid), scroll);
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
     * returns, so any idle-source delivery not yet dispatched never will be.
     * That is fine: phig_app_free stops and joins the loader's workers and
     * tears down the model explicitly, rather than waiting on those
     * deliveries or their refcounts to reach zero on their own. */
    g_object_unref(gtk_app);
    phig_app_free(app);
    return status;
}
