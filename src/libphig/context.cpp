#include "internal.h"
#include "paths.h"

extern "C" {

uint32_t phig_abi_version(void) { return (PHIG_ABI_VERSION_MAJOR << 16) | PHIG_ABI_VERSION_MINOR; }

phig_status phig_init(const phig_config* cfg, phig_ctx** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (cfg == nullptr || cfg->db_path == nullptr || cfg->db_path[0] == '\0') {
        return PHIG_ERR_INVALID_ARG;
    }
    try {
        auto ctx = std::make_unique<phig_ctx>();
        ctx->mode = cfg->mode;
        ctx->db_path = cfg->db_path;
        ctx->thumbs_dir = cfg->thumbs_dir ? cfg->thumbs_dir : "";
        // thumbs_dir is process-wide (see paths.h): phig resolves the cache
        // location through a global override, the same one the CLI sets from
        // config.ini. Only apply it when the caller supplied one, so a NULL
        // leaves any existing override untouched.
        if (cfg->thumbs_dir && cfg->thumbs_dir[0] != '\0') {
            set_path_overrides(std::nullopt, std::nullopt, std::string(cfg->thumbs_dir));
        }
        ctx->db = std::make_unique<Database>(cfg->db_path, cfg->mode == PHIG_READ_ONLY);
        // Read-only contexts must not migrate; a read-write one may.
        if (cfg->mode == PHIG_READ_WRITE) ctx->db->init_schema();
        *out = ctx.release();
        return PHIG_OK;
    } catch (...) {
        return PHIG_ERR_DB;
    }
}

void phig_shutdown(phig_ctx* ctx) { delete ctx; }

const char* phig_last_error(phig_ctx* ctx) { return ctx ? ctx->last_error.c_str() : ""; }

}  // extern "C"
