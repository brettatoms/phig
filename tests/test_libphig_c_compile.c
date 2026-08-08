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
    int busy : (PHIG_ERR_BUSY == 7) ? 1 : -1;
};

/* Same for the operation enums: a client compiled against one numbering and
 * run against another would misread every state and stage. */
struct phig_op_abi_pin {
    int running : (PHIG_OP_RUNNING == 0) ? 1 : -1;
    int done : (PHIG_OP_DONE == 1) ? 1 : -1;
    int cancelled : (PHIG_OP_CANCELLED == 2) ? 1 : -1;
    int failed : (PHIG_OP_FAILED == 3) ? 1 : -1;
    int file_done : (PHIG_STAGE_FILE_DONE == 0) ? 1 : -1;
    int walked : (PHIG_STAGE_WALKED == 1) ? 1 : -1;
    int selected : (PHIG_STAGE_SELECTED == 2) ? 1 : -1;
    int models : (PHIG_STAGE_LOADING_FACE_MODELS == 3) ? 1 : -1;
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
    cfg.models_dir = NULL;
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
                                512, path, sizeof(path), &exists);
    }

    {
        int64_t ids[2] = {1, 2};
        (void)phig_tag_add(ctx, ids, 2, "tag");
        (void)phig_tag_remove(ctx, ids, 2, "tag");
        (void)phig_gallery_set(ctx, ids, 2, "gallery");
        (void)phig_gallery_clear(ctx, ids, 2);
        (void)phig_favorite_set(ctx, ids, 2, true);
    }

    {
        phig_settings* st = NULL;
        (void)phig_settings_load(NULL, &st);
        (void)phig_settings_get(st, PHIG_KEY_DB, "fallback");
        (void)phig_settings_get_raw(st, PHIG_KEY_DB, "fallback");
        (void)phig_settings_get_int(st, PHIG_KEY_PARALLEL, 4);
        (void)phig_settings_get_bool(st, PHIG_KEY_LOG_ENABLED, true);
        (void)phig_settings_has(st, PHIG_KEY_THUMBS_DIR);
        (void)phig_settings_count(st);
        (void)phig_settings_key_at(st, 0);
        (void)phig_settings_set(st, PHIG_KEY_MODELS_DIR, "/models");
        (void)phig_settings_unset(st, PHIG_KEY_MODELS_DIR);
        (void)phig_settings_save(st, "/tmp/phig-c-compile-check.ini");
        (void)phig_settings_to_config(st, &cfg);
        phig_settings_free(st);
    }

    {
        static const char* const globs[1] = {"*.jpg"};
        phig_op* op = NULL;
        phig_op_progress prog;
        phig_index_options iopts;
        phig_thumbs_options topts;
        phig_faces_options fopts;
        phig_index_result ires;
        phig_thumbs_result tres;
        phig_faces_result fres;

        iopts.directory = "/nonexistent";
        iopts.recursive = true;
        iopts.include_hidden = false;
        iopts.force = false;
        iopts.media = PHIG_MEDIA_ANY;
        iopts.parallel = 0;
        (void)phig_op_index_start(ctx, &iopts, &op);

        topts.match = globs;
        topts.match_count = 1;
        topts.exclude = NULL;
        topts.exclude_count = 0;
        topts.size = 0;
        topts.force = false;
        topts.parallel = 0;
        (void)phig_op_thumbs_start(ctx, &topts, &op);

        fopts.match = NULL;
        fopts.match_count = 0;
        fopts.exclude = NULL;
        fopts.exclude_count = 0;
        fopts.parallel = 0;
        (void)phig_op_faces_start(ctx, &fopts, &op);

        (void)phig_op_status(op);
        (void)phig_op_poll(op, &prog);
        phig_op_cancel(op);
        phig_op_wait(op);
        (void)phig_op_index_result(op, &ires);
        (void)phig_op_thumbs_result(op, &tres);
        (void)phig_op_faces_result(op, &fres);
        (void)phig_op_error_count(op);
        (void)phig_op_error_at(op, 0);
        (void)phig_op_error(op);
        phig_op_free(op);
    }

    phig_shutdown(ctx);
}
