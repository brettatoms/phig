/* Compiles the public header as C99 to prove it contains no C++ constructs.
 * Also links every symbol so a missing definition fails the build. */
#include "phig/phig.h"

/* phig_status values are published ABI. Pin every one of them so a renumber
 * (rather than an append) fails the build instead of silently changing what a
 * compiled client sees. C99 has no static_assert, so use a negative-width
 * bitfield, which is a constraint violation the compiler must diagnose. */
struct phig_status_abi_pin {
    int ok : (PHIG_OK == 0) ? 1 : -1;
    int invalid_arg : (PHIG_ERR_INVALID_ARG == 1) ? 1 : -1;
    int not_found : (PHIG_ERR_NOT_FOUND == 2) ? 1 : -1;
    int db : (PHIG_ERR_DB == 3) ? 1 : -1;
    int readonly : (PHIG_ERR_READONLY == 4) ? 1 : -1;
    int internal : (PHIG_ERR_INTERNAL == 5) ? 1 : -1;
    int schema : (PHIG_ERR_SCHEMA == 6) ? 1 : -1;
};

void phig_c_compile_check(void) {
    phig_config cfg;
    phig_ctx* ctx = NULL;
    phig_query* q = NULL;
    phig_image_list* list = NULL;
    int64_t count = 0;
    char* json = NULL;

    cfg.db_path = "/nonexistent/phig.db";
    cfg.thumbs_dir = NULL;
    cfg.mode = PHIG_READ_ONLY;
    (void)phig_abi_version();
    (void)phig_init(&cfg, &ctx);
    (void)phig_last_error(ctx);

    (void)phig_query_new(ctx, &q);
    phig_query_set_extension(q, "jpg");
    phig_query_set_date_range(q, "2020-01-01", "2020-12-31");
    phig_query_set_camera(q, "model");
    phig_query_set_make(q, "make");
    phig_query_set_size_range(q, 0, 1);
    phig_query_set_media(q, PHIG_MEDIA_IMAGE);
    phig_query_set_duration_range(q, 0, 1);
    phig_query_add_tag(q, "tag");
    phig_query_set_gallery(q, "gallery");
    phig_query_set_sharpness(q, 0.0, 1.0);
    phig_query_set_sort(q, PHIG_SORT_DATE, true);
    (void)phig_query_count(ctx, q, &count);
    (void)phig_query_page(ctx, q, 0, 10, &list);
    phig_image_list_free(list);
    phig_query_free(q);

    (void)phig_image_metadata(ctx, 1, &json);
    phig_string_free(json);

    {
        char path[4096];
        bool exists = false;
        (void)phig_thumb_lookup(ctx,
                                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                path, sizeof(path), &exists);
    }

    {
        int64_t ids[2] = {1, 2};
        (void)phig_tag_add(ctx, ids, 2, "tag");
        (void)phig_tag_remove(ctx, ids, 2, "tag");
        (void)phig_gallery_set(ctx, ids, 2, "gallery");
        (void)phig_gallery_clear(ctx, ids, 2);
        (void)phig_favorite_set(ctx, ids, 2, true);
    }

    phig_shutdown(ctx);
}
