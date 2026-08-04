# phig-gtk

GTK4/libadwaita desktop client for [phig](../phig), a C++ image-collection CLI.
Talks to phig only through `libphig`'s C ABI (`<phig/phig.h>`) — never a
`phig-core` header, never a `cv::Mat`.

## Setup

Dependencies come from a Nix devenv and are not on the bare `PATH`. `.envrc` is
gitignored (matching phig's convention — `direnv allow` is per-machine), so a
fresh clone needs it recreated:

```bash
printf 'use devenv\n' > .envrc
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
