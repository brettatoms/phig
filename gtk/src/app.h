#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <memory>
#include <string>

#include "collection.h"
#include "gallery-model.h"
#include "gref.h"
#include "thumb-loader.h"

// The filter popover's fields.
struct FilterFields {
    GtkMenuButton* button = nullptr;
    GtkEntry* tag = nullptr;
    GtkEntry* gallery = nullptr;
    GtkCheckButton* favorite = nullptr;
    GtkEntry* after = nullptr;
    GtkEntry* before = nullptr;
    GtkEntry* camera = nullptr;
    GtkEntry* make = nullptr;
    GtkDropDown* media = nullptr;
    GtkEntry* extension = nullptr;
};

// What the content area's status page offers, when it shows instead of the
// grid.
enum class Status { NoPhotos, NoMatches, Error };

struct PhigApp {
    AdwApplicationWindow* window = nullptr;
    AdwToastOverlay* toast_overlay = nullptr;
    AdwNavigationView* nav = nullptr;
    AdwToolbarView* toolbar_view = nullptr;  // the gallery's content page
    GtkStack* content = nullptr;             // "grid" or "status"
    AdwStatusPage* status = nullptr;
    GtkButton* status_button = nullptr;
    Status status_kind = Status::NoPhotos;
    GtkListBox* sidebar = nullptr;
    GtkSearchEntry* search = nullptr;
    GtkDropDown* sort = nullptr;
    FilterFields filter;
    GtkFlowBox* chips = nullptr;
    GtkGridView* grid = nullptr;
    GtkScrolledWindow* scroller = nullptr;
    GtkLabel* failed_label = nullptr;  // header-bar counter for THUMB_FAILED results
    std::string catalog_dir;

    // Set once startup finishes. The loader's workers and the model's page
    // fetches both use coll from worker threads, so ~PhigApp stops both before
    // coll closes. coll is declared before loader, but the destructor body
    // releases the loader and drops the models first, then member destruction
    // closes coll.
    phig::gtk::CollectionPtr coll;
    std::shared_ptr<ThumbLoader> loader;

    // What the gallery shows, and the model built for it. query_seq counts
    // reloads, so a reload that finishes after a newer one started is dropped.
    phig::gtk::Query query;
    guint query_seq = 0;
    GRef<PhigGalleryModel> model;

    guint thumb_failed_count = 0;
    bool page_fetch_toast_shown = false;  // one toast per session, not one per failed page
    // An open is running. A second activation then builds a window and leaves
    // startup_done to fill it, rather than starting a second open whose result
    // would replace coll under the first's loader.
    bool opening = false;
    bool measured = false;  // PHIG_MEASURE's startup line has been printed

    gint64 startup_start_us = 0;  // g_get_monotonic_time() at process start, for PHIG_MEASURE

    // Zoom state. columns is the resting column count; pinch_cell tracks the
    // live size during a pinch so the snap can see which way it is going.
    guint target_cell_px = 0;            // what zoom sets; held across window resizes
    guint columns = 0;                   // derived from target_cell_px and the viewport width
    guint relayout_id = 0;               // pending phig_relayout_idle, 0 if none
    bool relayout_needs_rebind = false;  // geometry changed: re-request textures
    double pinch_cell = 0.0;             // live target size during a pinch
    guint pinch_base_cell = 0;           // target size when the gesture began

    PhigApp() = default;
    PhigApp(const PhigApp&) = delete;
    PhigApp& operator=(const PhigApp&) = delete;
    ~PhigApp();
};

// main.cpp
void show_toast(PhigApp& app, const std::string& text);
void open_catalog(PhigApp& app);  // opens app.catalog_dir in the background, then startup_done

// browse.cpp: the sidebar, the header's search, sort and filter, the chips and
// the status page.
GtkWidget* sidebar_new(PhigApp& app);
void header_fill(PhigApp& app, AdwHeaderBar* header);
GtkWidget* chips_new(PhigApp& app);
GtkWidget* status_new(PhigApp& app);
void browse_set_sensitive(PhigApp& app, bool sensitive);
void show_status(PhigApp& app, Status status, const std::string& message = {});
// Reads the ids for app.query on a worker and attaches a model for them, and
// refreshes the sidebar's counts.
void reload(PhigApp& app);
