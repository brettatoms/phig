/* Consumes the INSTALLED libphig as a plain C client. */
#include <phig/phig.h>
#include <stdio.h>

int main(void) {
    if ((phig_abi_version() >> 16) != PHIG_ABI_VERSION_MAJOR) {
        fprintf(stderr, "ABI major mismatch\n");
        return 1;
    }
    phig_config cfg = {0};
    phig_ctx* ctx = NULL;
    cfg.db_path = "/nonexistent/phig.db";
    cfg.mode = PHIG_READ_ONLY;
    if (phig_init(&cfg, &ctx) == PHIG_OK) {
        fprintf(stderr, "expected init failure on bad path\n");
        phig_shutdown(ctx);
        return 1;
    }
    phig_settings* st = NULL;
    phig_status sst = phig_settings_load("/nonexistent/phig-config.ini", &st);
    if (sst != PHIG_ERR_NOT_FOUND || st == NULL) {
        fprintf(stderr, "expected NOT_FOUND with a usable handle\n");
        return 1;
    }
    if (phig_settings_count(st) != 0) {
        fprintf(stderr, "expected an empty settings handle\n");
        return 1;
    }
    phig_settings_free(st);

    printf("ok\n");
    return 0;
}
