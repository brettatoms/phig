---
name: Icon and Desktop Entry
status: completed
created: 2026-11-14
completed: 2026-11-14
description: >
  Give phig an application icon, a halved fig drawn for the project as one
  master SVG plus a tiled variant, and give the GTK client an XDG desktop
  entry and install rules so it appears in application launchers. The macOS
  client builds its icon from the tiled variant. Excludes fitting the tile to
  Apple's grid and the macOS asset catalog, which belong to that client.
---

# 074 — Icon and Desktop Entry

## Problem

The GTK client has no icon and no desktop entry, so a launcher cannot show or
start it, and nothing installs the binary.

## Decisions

1. **The icon is an original drawing**, a halved fig in a flat style: dark
   purple skin, a pink rind, a cream inner band, a peach band, red flesh with
   orange seeds and a dark red centre slit, and the right half shaded darker.
   It has no stem. The user chose it on 2026-11-14 from four drafts (a flat
   half, the same on a tile, a shaded half, and a public-domain Icon Archive
   fig) and nine whole-fig drafts in five purples. The user's first reference
   was a Vecteezy illustration. It was not traced or copied: its Free License
   requires attribution, its Pro license ends with the subscription and
   forbids keeping the file afterwards, and both forbid distributing the
   content as a source file, which a public repository does.
2. **One master SVG at `assets/icon/phig.svg`**, artwork only on a transparent
   background, shared by every client. It renders legibly from 16 px up.
3. **A tiled variant at `assets/icon/phig-tile.svg`**: the same fig scaled to
   74% with a soft drop shadow on a light blue rounded square. It is the
   starting point for the macOS icon. The tile's colour is a placeholder, and
   its proportions are not yet fitted to Apple's icon grid.
4. **Linux:** the master installs as
   `share/icons/hicolor/scalable/apps/org.phig.Gtk.svg`, named for the app id
   the client registers (`org.phig.Gtk`), and `gtk/data/org.phig.Gtk.desktop`
   installs to `share/applications/`. CMake `install()` rules install those
   and the `phig-gtk` binary. GNOME and KDE draw application icons without a
   tile, so Linux uses the master, not the tiled variant.
5. **macOS** builds its icon from the tiled variant, fitted to Apple's grid,
   as part of the macOS client.

6. **The desktop entry** names the app `phig`, launches `phig-gtk`, and
   lists `Categories=Graphics;Photography;Viewer;` with keywords for photo,
   image, picture, gallery, collection and duplicates.
7. **No symbolic icon for now.** Launchers and docks use the full-colour
   icon. A one-colour `org.phig.Gtk-symbolic.svg` can be added when
   something needs it.
8. **Only the SVGs are committed**, not the script that generated the drafts.
   It placed the seeds at random, and the SVG is edited directly from here.

## Testing

- The SVG renders legibly at 16, 32, 64 and 256 px, checked by eye.
- `desktop-file-validate` passes on the desktop entry.
- `cmake --install` into a scratch prefix places the binary, the desktop entry
  and the icon where the XDG specs expect them.

## Outcome

Built as decided, 2026-11-14. `assets/icon/phig.svg` and
`assets/icon/phig-tile.svg` hold the icon, `gtk/data/org.phig.Gtk.desktop` the
entry, and `gtk/CMakeLists.txt` installs the binary, the entry and the master
icon as `org.phig.Gtk.svg`. `gtk/README.md` says how to install into `~/.local`.

Checked: the master renders legibly at 16, 24, 32, 48 and 256 px;
`desktop-file-validate` passes; `cmake --install` into a scratch prefix placed
`bin/phig-gtk`, `share/applications/org.phig.Gtk.desktop` and
`share/icons/hicolor/scalable/apps/org.phig.Gtk.svg`, and the installed binary
resolves all its libraries. Whether a desktop shows the icon in its launcher
and on the running window is not checked here; it needs an install into a
prefix the desktop reads.
