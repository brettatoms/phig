#include <filesystem>

#include "catalog.h"
#include "internal.h"
#include "paths.h"

std::string& phig_thread_last_error() {
    static thread_local std::string error;
    return error;
}

extern "C" {

uint32_t phig_abi_version(void) { return (PHIG_ABI_VERSION_MAJOR << 16) | PHIG_ABI_VERSION_MINOR; }

phig_status phig_init(const phig_config* cfg, phig_ctx** out) {
    if (out == nullptr) return PHIG_ERR_INVALID_ARG;
    *out = nullptr;
    if (cfg == nullptr || cfg->catalog_dir == nullptr || cfg->catalog_dir[0] == '\0') {
        return PHIG_ERR_INVALID_ARG;
    }
    // Checked before set_catalog and the open: an unknown mode must not reach the
    // read-write path, which creates a missing catalog (R73).
    if (cfg->mode != PHIG_READ_ONLY && cfg->mode != PHIG_READ_WRITE) return PHIG_ERR_INVALID_ARG;
    try {
        const CatalogPaths paths =
            catalog_paths(std::filesystem::absolute(cfg->catalog_dir).lexically_normal().string());
        set_catalog(paths);
        migrate_legacy_catalog_files(paths);
        auto ctx = std::make_unique<phig_ctx>();
        ctx->mode = cfg->mode;
        ctx->db_path = paths.db;
        // A read-only context neither creates nor migrates (R70); a read-write
        // one creates a missing catalog, because a client indexes into a new
        // catalog through one.
        ctx->db = open_catalog_db(
            paths.db, cfg->mode == PHIG_READ_ONLY ? CatalogAccess::Read : CatalogAccess::Create);
        *out = ctx.release();
        return PHIG_OK;
    } catch (const CatalogError& e) {
        return e.kind() == CatalogError::Kind::Missing ? PHIG_ERR_DB : PHIG_ERR_SCHEMA;
    } catch (...) {
        return PHIG_ERR_DB;
    }
}

void phig_shutdown(phig_ctx* ctx) { delete ctx; }

const char* phig_last_error(phig_ctx* ctx) { return ctx ? phig_thread_last_error().c_str() : ""; }

}  // extern "C"
