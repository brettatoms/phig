#ifndef PHIG_GTK_FAKE_COLLECTION_H
#define PHIG_GTK_FAKE_COLLECTION_H

#include "collection.h"

G_BEGIN_DECLS

/* A PhigCollection with n_rows synthetic rows and no database. Row i (0-based)
 * has id i + 1, path "/fake/<i>.jpg", filename "<i>.jpg" and the sha256
 * fake_collection_sha gives for i. Thumbnails live in a private temp directory
 * that phig_collection_close removes. Generating one writes a 64x64 PNG. Generation fails for
 * an id outside [1, n_rows]. */
PhigCollection* fake_collection_new(guint n_rows);

/* Row index's sha256: the index in hex, zero-padded to 64 digits. */
void fake_collection_sha(guint index, char out[65]);

/* While fail is set, phig_collection_page returns NULL. */
void fake_collection_fail_pages(PhigCollection* coll, gboolean fail);

/* How many thumbnail files phig_collection_thumb_generate has written. */
guint fake_collection_generate_count(PhigCollection* coll);

/* While hold is set, phig_collection_thumb_generate waits before generating. */
void fake_collection_hold_generation(PhigCollection* coll, gboolean hold);

G_END_DECLS

#endif /* PHIG_GTK_FAKE_COLLECTION_H */
