#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>

#include "gallery-model.h"

/* PhigGallerySelection: the gallery grid's GtkSelectionModel. It selects
 * positions, not items. In a PhigGalleryModel a position names the same image
 * for the model's whole life, but the item at a position is replaced whenever
 * its page lands or is fetched again, and a selection that tracked items would
 * lose those positions without saying so.
 *
 * It forwards the wrapped model's items and items-changed. Selecting never asks
 * for an item, so select all fetches no page. The read-only "model" property
 * is the wrapped gallery model. */
#define PHIG_TYPE_GALLERY_SELECTION (phig_gallery_selection_get_type())
G_DECLARE_FINAL_TYPE(PhigGallerySelection, phig_gallery_selection, PHIG, GALLERY_SELECTION, GObject)

/* Takes its own reference to model. Starts with nothing selected. */
PhigGallerySelection* phig_gallery_selection_new(PhigGalleryModel* model);
