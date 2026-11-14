#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <functional>
#include <memory>
#include <string>

#include "collection.h"
#include "gallery-model.h"
#include "gref.h"
#include "scan-watch.h"
#include "thumb-loader.h"

// The viewer page: one image, its metadata and the edits it offers.
struct Viewer {
    AdwNavigationPage* page = nullptr;
    GtkPicture* picture = nullptr;
    GtkButton* prev = nullptr;
    GtkButton* next = nullptr;
    GtkButton* favorite = nullptr;
    GtkLabel* filename = nullptr;
    GtkLabel* date = nullptr;
    GtkLabel* camera = nullptr;
    GtkLabel* exposure = nullptr;
    GtkLabel* size = nullptr;
    GtkLabel* sharpness = nullptr;
    GtkFlowBox* tags = nullptr;
    GtkEntry* tag_entry = nullptr;
    GtkEntry* gallery_entry = nullptr;

    // The model the viewer steps through. Its own reference, so a gallery
    // reload does not move the image under it.
    GRef<PhigGalleryModel> model;
    guint position = 0;
    int64_t id = 0;
    // Bumped on every image shown. A decode, details read or thumbnail that
    // finishes for an older one is dropped.
    guint seq = 0;
    bool full_shown = false;  // the decode for seq has landed; a thumbnail must not replace it
    bool details_loaded = false;
    phig::gtk::Details details;       // of id, once details_loaded
    GRef<GCancellable> thumb_cancel;  // the thumbnail request for seq
    guint details_seq = 0;            // bumped per details read; an older read's result is dropped
    bool decode_running = false;      // at most one full decode is in flight
};

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

// What the content area's status page offers, when it shows instead of the grid.
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
    GtkActionBar* selection_bar = nullptr;
    GtkLabel* selection_label = nullptr;
    GtkEntry* selection_tag = nullptr;
    GtkEntry* selection_gallery = nullptr;
    GtkButton* scan_button = nullptr;
    GtkWidget* scan_bar = nullptr;
    GtkLabel* scan_label = nullptr;
    GtkProgressBar* scan_progress = nullptr;
    GtkButton* scan_cancel_button = nullptr;
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
    GtkSelectionModel* selection = nullptr;  // the grid's; owned by the grid
    Viewer viewer;
    phig::gtk::ScanWatchPtr scan;  // the scan running, if any
    std::string scan_folder;

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
void launch_file(PhigApp& app, const std::string& path);
void open_catalog(PhigApp& app);  // opens app.catalog_dir in the background, then startup_done
// PHIG_MEASURE's scroll harness; reload starts it when the first model attaches.
void phig_measure_maybe_start(PhigApp* app);

// browse.cpp: the sidebar, the header's search, sort and filter, the chips,
// the status page and the selection bar.
GtkWidget* sidebar_new(PhigApp& app);
void header_fill(PhigApp& app, AdwHeaderBar* header);
GtkWidget* chips_new(PhigApp& app);
GtkWidget* status_new(PhigApp& app);
GtkWidget* selection_bar_new(PhigApp& app);
void browse_set_sensitive(PhigApp& app, bool sensitive);
void show_status(PhigApp& app, Status status, const std::string& message = {});
// Reads the ids for app.query on a worker and attaches a model for them, and
// refreshes the sidebar's counts.
void reload(PhigApp& app);
void update_selection_bar(PhigApp& app);

// Runs a metadata write against the collection on a worker. On success the
// sidebar's counts refresh, the gallery reloads if the write can change what
// it shows, and the viewer re-reads its image. On failure a toast says why and
// that nothing changed.
void run_write(
    PhigApp& app,
    std::move_only_function<phig::gtk::WriteResult(phig::gtk::Collection&) noexcept> write);

// viewer.cpp
void viewer_build(PhigApp& app);
void viewer_open(PhigApp& app, guint position);
void viewer_refresh(PhigApp& app);  // re-reads the shown image's metadata

// scan-ui.cpp
GtkWidget* scan_bar_new(PhigApp& app);
void scan_dialog_present(PhigApp& app);
// Scan… (header and status page) is sensitive unless a scan runs or the catalog is opening.
void scan_update_sensitivity(PhigApp& app);
