// Plan 048 — HEIC decode benchmark: libheif (software) vs ImageIO (hardware).
//
// Measures the decision behind 048's platform split for pixels. Pair with
// 2026-08-08-048-heic-decode-bench.swift, which measures the same file
// through ImageIO. Both force a full decode; neither times file I/O only.
//
//   cc -O2 2026-08-08-048-heic-decode-bench.c -o bench_heif \
//      -I/opt/homebrew/include -L/opt/homebrew/lib -lheif
//   ./bench_heif <file.heic> 8
//
// Measured 2026-08-08, MacBook Air M3 / 16 GB / macOS 26, on a 3840x2160
// 4:4:4 HEIC: libheif best 188.4 ms wall, 0.64 s CPU per decode (it uses
// ~3.3 cores). ImageIO best 41.6 ms wall, 0.047 s CPU. See plans/048.

#include <libheif/heif.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char** argv) {
    int n = atoi(argv[2]);
    double best = 1e9;
    for (int i = 0; i < n; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct heif_context* ctx = heif_context_alloc();
        heif_context_read_from_file(ctx, argv[1], NULL);
        struct heif_image_handle* h = NULL;
        heif_context_get_primary_image_handle(ctx, &h);
        struct heif_image* img = NULL;
        struct heif_error e = heif_decode_image(h, &img, heif_colorspace_RGB,
                                                heif_chroma_interleaved_RGB, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (e.code != heif_error_Ok) { printf("ERROR: %s\n", e.message); return 1; }
        double ms = (t1.tv_sec-t0.tv_sec)*1e3 + (t1.tv_nsec-t0.tv_nsec)/1e6;
        if (ms < best) best = ms;
        printf("%.0f ", ms); fflush(stdout);
        heif_image_release(img); heif_image_handle_release(h); heif_context_free(ctx);
    }
    printf("\nlibheif best: %.1f ms\n", best);
    return 0;
}
