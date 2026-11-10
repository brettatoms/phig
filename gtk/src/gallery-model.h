#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#include <cstdint>

#include "collection.h"

/* PhigImageRow: one row of the gallery. It owns copies of its fields, so a row
 * stays valid after its page is evicted.
 *
 * A row can be a placeholder: returned by PhigGalleryModel for a page that
 * has not been fetched yet, so get_item never blocks. Check
 * phig_image_row_is_placeholder before trusting the other getters. */
#define PHIG_TYPE_IMAGE_ROW (phig_image_row_get_type())
G_DECLARE_FINAL_TYPE(PhigImageRow, phig_image_row, PHIG, IMAGE_ROW, GObject)

gboolean phig_image_row_is_placeholder(PhigImageRow* row);
int64_t phig_image_row_get_id(PhigImageRow* row);
const char* phig_image_row_get_path(PhigImageRow* row);
const char* phig_image_row_get_sha256(PhigImageRow* row);
const char* phig_image_row_get_filename(PhigImageRow* row);

/* PhigGalleryModel: a GListModel over the collection's gallery rows without holding
 * the collection in memory. It knows the total row count up front (from
 * phig::gtk::count) and keeps only an LRU of fetched pages resident.
 *
 * get_item never blocks: a resident page returns a ref to its cached row; a
 * non-resident page returns a placeholder row and schedules an async fetch
 * (a GTask running phig::gtk::page on a worker thread), which installs the
 * page and emits items-changed for its range from the GTask completion
 * callback on the main thread. Pages beyond the resident budget are evicted
 * least-recently-used; rows already handed out to callers stay valid because
 * they are refcounted GObjects owning their own copies.
 *
 * Signals:
 *   "page-fetch-failed" (const char* reason): a page fetch failed, for the
 *   reason the collection gave; the affected positions keep their placeholder
 *   rows (no items-changed is emitted for them, since nothing changed).
 *   Emitted once per failed fetch, on the main thread; a GUI is expected to
 *   coalesce this into one user-visible notification rather than one per
 *   occurrence. */
#define PHIG_TYPE_GALLERY_MODEL (phig_gallery_model_get_type())
G_DECLARE_FINAL_TYPE(PhigGalleryModel, phig_gallery_model, PHIG, GALLERY_MODEL, GObject)

/* Caller keeps ownership of coll and must not close it while any reference to
 * the model is outstanding: page fetches use it from a worker thread for as
 * long as the model exists. */
PhigGalleryModel* phig_gallery_model_new(phig::gtk::Collection& coll);

/* Test-only helpers. Production code goes through GListModel's get_item,
 * which schedules an async fetch; these exist so tests can force a page to
 * become resident, and inspect the resident-page budget, without a
 * GMainLoop. */
void phig_gallery_model_fetch_page_sync(PhigGalleryModel* self, guint page_index);
guint phig_gallery_model_resident_page_count(PhigGalleryModel* self);
guint phig_gallery_model_get_max_resident_pages(PhigGalleryModel* self);
