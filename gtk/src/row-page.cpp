#include "collection.h"

PhigRowPage* phig_row_page_new(guint n_rows) {
    PhigRowPage* page = g_new0(PhigRowPage, 1);
    page->n_rows = n_rows;
    page->rows = g_new0(PhigRowData, n_rows);
    return page;
}

void phig_row_page_free(PhigRowPage* page) {
    if (page == NULL) return;
    for (guint i = 0; i < page->n_rows; i++) {
        g_free(page->rows[i].path);
        g_free(page->rows[i].sha256);
        g_free(page->rows[i].filename);
    }
    g_free(page->rows);
    g_free(page);
}
