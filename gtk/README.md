# phig-gtk

GTK4/libadwaita desktop client for [phig](../phig), a C++ image-collection CLI.
Talks to phig only through `libphig`'s C ABI (`<phig/phig.h>`) — never a
`phig-core` header, never a `cv::Mat`.

## Setup

Dependencies come from a Nix devenv and are not on the bare `PATH`. `.envrc` is
gitignored (matching phig's convention — `direnv allow` is per-machine), so a
fresh clone needs it recreated:

```bash
cat > .envrc <<'EOF'
#!/usr/bin/env bash

export DIRENV_WARN_TIMEOUT=20s

eval "$(devenv direnvrc)"

use devenv
EOF
direnv allow
```

phig-gtk links against `libphig`, discovered via `pkg-config`. Install phig to
`~/.local` (outside both repos) before the first build:

```bash
cd ../phig && direnv exec . cmake --install build --prefix ~/.local
```

## Build

```bash
direnv exec . cmake -B build && direnv exec . cmake --build build -j
direnv exec . ./build/phig-gtk
```

## Measured performance (Task 7)

Measured against the real collection (`~/.local/share/phig/phig.db`, 29,953
rows) with a Wayland session, on this machine (12 cores, NVMe thumbnail
cache, HDD-backed source photos). Startup is 5 runs (median shown); frame
timing is via `gtk_widget_add_tick_callback` on the grid, driven by a
`PHIG_MEASURE=1` scroll harness (see `src/main.c`) since no input-injection
path exists in this environment.

| Metric | Value |
|---|---|
| Startup (median of 5) | 73.8 ms (range 67.1-77.4 ms) |
| Worst frame, warm cache, full-range scroll | 66.7-83.3 ms across runs |
| Frames over 16 ms, warm cache | ~95% of frames |
| Worst frame, miss-heavy scroll (~5,500-row gap) | ~1.3 s |
| Peak RSS, warm scroll | ~366-480 MB (varies with thread/cache settings) |
| Peak RSS, miss-heavy scroll | ~460-670 MB |
| GTK warnings, full-range fast-drag scroll | 0 |

Chosen values (see comments in `src/main.c` and `src/gallery-model.c` for the
measurements behind each):

| Value | Chosen | Rationale |
|---|---|---|
| `PHIG_PAGE_RADIUS` | 2 | 1/2/4 measured identical frame timing and comparable RSS; page-fetch latency is never the bottleneck. |
| `PHIG_THUMB_LOADER_THREADS` | 4 | 2/4/8 measured identical frame timing; 8 also reproduced an unrelated pre-existing crash (see below). |
| `PHIG_THUMB_CACHE_BYTES` | 256 MB | 64/256/512 MB measured identical frame timing; RSS scales with budget (276/476/692 MB peak). 256 MB kept for scrollback headroom a one-way sweep can't measure. |
| `PHIG_MAX_PLACEHOLDERS` | derived (1000) | Zero "Duplicate item detected" warnings under fast full-range drags, warm and miss-heavy. |

**M0 criterion: window under 500 ms, 60 fps scrolling the full grid with no
stalls.** Startup meets its budget (73.8 ms median, well under 500 ms).
Scrolling does not: even on a fully warm cache, ~95% of frames exceed 16 ms
(worst observed 83.3 ms), so the grid does not hold 60 fps. A miss-heavy
scroll is far worse (worst frame ~1.3 s), which is expected — thumbnail
generation reads the HDD-backed source at roughly 13 files/sec — but the warm
case alone already misses the criterion. None of the four tuned values
change this: they affect RSS and (for threads) HDD-bound miss handling, not
the warm-cache frame time, which is dominated by main-thread texture upload
and widget work that this task's scope did not change. Full numbers and the
crash finding are in `.superpowers/sdd/2026-08-04-phig-gtk-m0/task-7-report.md`.
