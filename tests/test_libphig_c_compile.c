/* Compiles the public header as C99 to prove it contains no C++ constructs.
 * Also links every symbol so a missing definition fails the build. */
#include "phig/phig.h"

void phig_c_compile_check(void) {
    phig_config cfg;
    phig_ctx* ctx = NULL;
    cfg.db_path = "/nonexistent/phig.db";
    cfg.thumbs_dir = NULL;
    cfg.mode = PHIG_READ_ONLY;
    (void)phig_abi_version();
    (void)phig_init(&cfg, &ctx);
    (void)phig_last_error(ctx);
    phig_shutdown(ctx);
}
