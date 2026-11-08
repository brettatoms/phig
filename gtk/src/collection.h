#ifndef PHIG_GTK_COLLECTION_H
#define PHIG_GTK_COLLECTION_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* The gtk client's only route into phig. src/collection.cpp implements it over
 * core/; the tests link tests/fake-collection.cpp instead. Every function may
 * be called from any thread and never lets an exception escape.
 *
 * Reads are paged: no call returns the whole collection. A full-collection
 * read through core/ carries every row's EXIF blob, about 3.8 KB a row that
 * the client never reads. */
typedef struct PhigCollection PhigCollection;

/* One gallery row. Strings belong to the PhigRowPage holding the row. */
typedef struct {
    int64_t id;
    char* path;
    char* sha256; /* "" when phig has not hashed the row */
    char* filename;
} PhigRowData;

typedef struct {
    PhigRowData* rows;
    guint n_rows;
} PhigRowPage;

/* For implementations of this interface: a page of n_rows zeroed rows. */
PhigRowPage* phig_row_page_new(guint n_rows);
void phig_row_page_free(PhigRowPage* page);

/* Opens the database at db_path read-only. Returns NULL and sets *error (free
 * with g_free) if the file is missing or unreadable, or is at a schema version
 * this build does not read. Never creates a file. */
PhigCollection* phig_collection_open(const char* db_path, char** error);
void phig_collection_close(PhigCollection* coll);

/* phig's default database path ($XDG_DATA_HOME/phig/phig.db). Free with g_free. */
char* phig_collection_default_path(void);

/* Rows are in gallery order: newest effective date first, id breaking ties. */
gboolean phig_collection_count(PhigCollection* coll, guint* out);

/* Rows [offset, offset + limit) in gallery order. NULL on failure; a page past
 * the end has n_rows == 0. */
PhigRowPage* phig_collection_page(PhigCollection* coll, guint offset, guint limit);

typedef enum {
    PHIG_THUMB_PRESENT, /* the file at *path exists */
    PHIG_THUMB_ABSENT,  /* *path is where it would be; generate to create it */
    PHIG_THUMB_ERROR    /* sha256 or size is not valid; *path is NULL */
} PhigThumbLookup;

/* Where the cached thumbnail for (sha256, size) is. Sets *path (free with
 * g_free) unless the result is PHIG_THUMB_ERROR. */
PhigThumbLookup phig_collection_thumb_lookup(PhigCollection* coll, const char* sha256, int size,
                                             char** path);

/* Makes sure row image_id has a cached thumbnail at size, generating it if
 * absent. FALSE if the row is unknown or unhashed, or its file is missing or
 * cannot be decoded. */
gboolean phig_collection_thumb_generate(PhigCollection* coll, int64_t image_id, int size);

G_END_DECLS

#ifdef __cplusplus
#include <memory>

struct PhigCollectionClose {
    void operator()(PhigCollection* coll) const noexcept { phig_collection_close(coll); }
};
using CollectionPtr = std::unique_ptr<PhigCollection, PhigCollectionClose>;
#endif

#endif /* PHIG_GTK_COLLECTION_H */
