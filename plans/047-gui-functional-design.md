---
name: GUI Functional Design
status: designing
created: 2026-08-12
description: >
  Functional screen-and-navigation design for three desktop clients — GTK4
  (Linux), SwiftUI/AppKit (macOS), WinUI 3 (Windows) — covering all phig
  features through 9 screens with progressive disclosure.
---

# 047 — GUI Functional Design

## What this is

A functional screen-and-navigation design for the phig desktop applications,
covering the GTK4/libadwaita client (Linux), a macOS client (SwiftUI/AppKit),
and a Windows client (WinUI 3). The three share the same feature surface and
screen inventory but use platform-native idioms. This design covers every
current and planned phig feature, organized to avoid clutter through progressive
disclosure — the common-case surface is three screens, and everything else is
one action away.

This is not a wireframe document or a pixel-level spec. It describes what each
screen does, what the user can do on it, and how screens relate to each other.
Platform differences are noted inline and summarized in a table at the end.

## Design principles

1. **The grid is home.** Gallery browsing is the default screen and the return
   point. Everything else is either a filter on the grid (search, duplicates,
   people) or a transient operation (organize review, job progress).
2. **Platform-native, not ported.** The three apps share screen inventory and
   information architecture. They do not share widget trees or layout code.
3. **Progressive disclosure.** The first-launch surface is a gallery grid and a
   search field. Tags, faces, duplicates, organize, and activity are reached
   through the sidebar or toolbar, not presented up front.
4. **Review before mutate.** Every destructive or file-moving operation shows a
   preview of what will happen, with per-item checkboxes, before the user
   confirms.
5. **Jobs are backgrounded.** Scan, thumbnail rebuild, face detection, and
   organize are long-running jobs that show progress in a non-modal bar; the
   gallery remains browsable while they run.

## Navigation model

```
┌──────────────────────────────────────────────────────────────┐
│  [sidebar]    │           content area                       │
│               │  ┌───────────────────────────────────────┐   │
│  All Photos   │  │ toolbar / header bar                  │   │
│  Favorites    │  │  [search] [filter chips] [view mode]  │   │
│  Galleries  ▸ │  ├───────────────────────────────────────┤   │
│  People     ▸ │  │                                       │   │
│  Tags       ▸ │  │   grid / viewer / detail page         │   │
│  Duplicates   │  │                                       │   │
│  ─────────── │  │                                       │   │
│  Scan...      │  │                                       │   │
│  Organize...  │  ├───────────────────────────────────────┤   │
│  Activity Log │  │ status bar / job progress             │   │
│  ─────────── │  └───────────────────────────────────────┘   │
│  Settings     │             [optional inspector pane]        │
└──────────────────────────────────────────────────────────────┘
```

### Sidebar sections

**Browse** (top group):
- **All Photos** — the unfiltered gallery, sorted by date descending. The default.
- **Favorites** — gallery filtered to `favorite = true`.
- **Galleries** — expands to show the user's gallery names; picking one filters
  the grid to that gallery.
- **People** — expands to show named people; picking one filters the grid to
  photos containing that person.
- **Tags** — expands to show tag names; picking one filters the grid to that tag.
  Selecting multiple tags is AND (photo has all of them).
- **Duplicates** — switches to a dedicated duplicates review screen (not a grid
  filter).

**Actions** (middle group):
- **Scan…** — opens the scan dialog.
- **Organize…** — opens the organize (cp/mv) dialog.
- **Activity Log** — opens the audit trail (specialty screen).

**System** (bottom group):
- **Settings** — opens preferences.

### Platform difference: navigation

| | GTK | macOS | Windows |
|---|---|---|---|
| Pattern | `AdwNavigationSplitView` with sidebar + content | `NavigationSplitView` (three-column: sidebar, content, optional inspector) | `NavigationView` with hamburger menu (pane + content); back button in title bar for sub-pages |
| Sidebar | Flat list with section headers; expandable rows for Galleries/People/Tags | Source list (`SidebarListStyle`) with disclosure triangles; section headers | Navigation menu with section headers ("Browse", "Actions", "System"); expandable `NavigationViewItem` with `IconSource` for Galleries/People/Tags |
| Inspector | Not a separate column; metadata shown in a bottom sheet or popover on selection | Third column when useful (image metadata, tag editor on selection) | Right-hand pane or `TeachingTip`/flyout on selection; not a permanent third column |
| Adaptive | `AdwBreakpoint` collapses sidebar at narrow widths; bottom navigation bar on phone-sized | Sidebar toggles with toolbar button at narrow widths; tab bar on iPhone | `NavigationView` auto-collapses to hamburger at narrow widths; `TabView` on compact |
| Back navigation | `AdwNavigationPage` push/pop (swipe-back gesture) | `NavigationLink` push; swipe-back gesture on trackpad | Back button in title bar; browser-style back mouse button (XButton1) |

## Screen 1: Gallery (home)

The primary screen. A scrollable, zoomable thumbnail grid with filter chips and a
search field.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  ◀ ▶  [🔍 search or filter...          ]  [≡ sort] [⊙ zoom] │  ← header bar / toolbar
├──────────────────────────────────────────────────────────────┤
│  [All Photos] [Favorites] [2026] [Aug] [📷 Canon]  …        │  ← active filter chips
├──────────────────────────────────────────────────────────────┤
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐                 │
│  │    │ │    │ │    │ │    │ │    │ │    │                 │
│  │    │ │    │ │    │ │    │ │    │ │    │                 │
│  └────┘ └────┘ └────┘ └────┘ └────┘ └────┘                 │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐                 │  ← virtualized grid
│  │    │ │    │ │    │ │    │ │    │ │    │                 │
│  │    │ │    │ │    │ │    │ │    │ │    │                 │
│  └────┘ └────┘ └────┘ └────┘ └────┘ └────┘                 │
│  …                                                          │
├──────────────────────────────────────────────────────────────┤
│  ✓ 12 of 29,953 selected   [Tag...] [Gallery...] [☆] [🗑]  │  ← selection bar (multi-select)
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

**Thumbnail grid:**
- Virtualized, page-fetched. Already shipped in M0; same model for all platforms.
- Pinch-to-zoom and Ctrl+scroll change the column count (M0's zoom policy: derived
  cell size, no horizontal scroll, direction-dependent hysteresis).
- Click a thumbnail → opens the Viewer.
- Right-click / long-press a thumbnail → context menu (Open, Favorite, Tag…,
  Add to Gallery, Show in File Manager, Copy Path, Delete…).
- Multi-select: click selects, Shift+click ranges, Ctrl+click toggles. Selected
  count and actions appear in the selection bar at the bottom.

**Filter chips:**
- Below the toolbar, a horizontal row of active filters rendered as removable
  chips: "Tag: vacation", "Camera: Canon EOS R5", "Date: Aug 2026", "Person: Mom".
- Each chip has an × to remove that filter.
- Adding a filter from search or the sidebar appends a chip.
- Chips AND together.

**Search field:**
- Free-text search across filename, path, and EXIF metadata.
- As the user types, a dropdown suggests: matching tag names, person names,
  gallery names, camera models, and date ranges — selecting one adds a chip.
- An explicit "Search" submit runs the full-text query.

**Sort control:**
- Dropdown: Date (default, newest first), Date (oldest first), Name, Size,
  Sharpness (blur score).
- Sort is a filter chip, not a hidden preference — the grid's sort order is
  always visible.

**Zoom control:**
- A slider or +/- buttons in the toolbar. Same behavior as pinch/Ctrl+scroll.

### Empty states

- **No images in collection:** Empty-state view: "No photos yet" with a button
  to Scan a directory.
- **Filter returns nothing:** "No photos match these filters" with a button to
  clear all filters.
- **Database missing/corrupt:** Error page with the message from libphig and a
  button to open settings.

### Platform notes

- **GTK:** `AdwHeaderBar` with a `GtkSearchEntry`, zoom buttons packed end.
  Filter chips in an `AdwWrapBox` below. Selection bar is a `GtkActionBar` that
  slides up from the bottom.
- **macOS:** Unified toolbar with `NSSearchField` in the center, zoom segmented
  control on the trailing edge. Filter chips as a `NSCollectionView` or
  `_NSTokenField`-like chip row. Selection bar as a footer view.
- **Windows:** Title bar with an `AutoSuggestBox` (search with dropdown
  suggestions) centered or left-aligned. Zoom via a `CommandBar` with +/- buttons
  or a slider in the `CommandBar` footer. Filter chips as a horizontal
  `ItemsRepeater` below the title bar. Selection bar as a `CommandBar` that
  appears at the bottom when items are selected ("12 selected — Tag, Gallery,
  Favorite, Delete").

---

## Screen 2: Viewer

Single-image display with metadata. Reached by clicking a thumbnail in the
gallery. On macOS, also reachable via Quick Look (spacebar) in the grid.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  ← Back to Gallery    [◀ prev] [next ▶]    [☆] [⋯]         │  ← toolbar
├──────────────────────────┬───────────────────────────────────┤
│                          │  📷 IMG_4628.CR2                  │
│                          │  Aug 12, 2026  14:32              │
│                          │  Canon EOS R5  ·  45.0 MP         │
│                          │  1/500  f/4.0  ISO 400  24mm      │
│                          │                                   │
│       image              │  Tags: [vacation] [+ add]         │
│                          │  Gallery: Travel 2026             │
│                          │  Person: Mom, Sarah               │
│                          │                                   │
│                          │  📍 San Francisco, CA             │
│                          │  ⚡ Sharp  (blur score: 0.94)     │
│                          │                                   │
│                          │  [Open in System Viewer]          │
│                          │  [Show in File Manager]           │
└──────────────────────────┴───────────────────────────────────┘
```

### Behaviors

**Image area:**
- Displays the full-resolution image, scaled to fit. Pan and zoom with
  mouse/trackpad.
- While the full decode loads (from phig-decode), shows the upscaled thumbnail —
  never a blank frame.
- Swipe left/right or arrow keys for next/previous image.
- Prefetches the next and previous images at viewer size for instant navigation.

**Metadata pane (right):**
- Filename, date, camera, exposure settings — from libphig's `phig_image`
  record.
- Tags: rendered as chips. Click × to remove. "+" button to add a tag (inline
  autocomplete from existing tags).
- Gallery: dropdown of existing galleries. A photo belongs to one gallery at a
  time (phig's constraint); selecting "(none)" clears it.
- People: faces in this photo shown as named chips. Unnamed faces shown as
  "Unknown" with a name-this-person prompt.
- GPS: city/region name if geocoded (plan 013). Coordinates as secondary text.
- Blur score: shown as a colored bar/pill (green if sharp, yellow if marginal,
  red if blurry).
- Quick actions at the bottom.

**Favorite star:**
- In the toolbar, toggles favorite on/off. Persisted immediately through libphig.

**Context menu** (right-click on image):
- Same actions as grid context menu: Tag, Gallery, Favorite, Show in Finder,
  Copy Path, Delete…

### Platform notes

- **GTK:** Metadata in a right-hand pane or a bottom sheet. Back navigation
  through `AdwNavigationPage` push/pop (provides swipe-back gesture).
- **macOS:** Three-column split view: sidebar, image, inspector. Inspector
  contains metadata, tags, and actions. Arrow-key navigation built in. Quick
  Look integration: selecting a thumbnail in the grid and pressing spacebar
  opens the macOS Quick Look panel; double-click opens the in-app viewer.
- **Windows:** Viewer is a sub-page pushed onto the `NavigationView` content
  frame (back button in title bar returns to gallery). Metadata in a right-hand
  pane or an `Expander` below the image on narrow layouts. Preview handler
  registration: thumbnails in File Explorer can preview via phig's thumbnail
  cache (out of scope for initial release but architecturally supported).

---

## Screen 3: Duplicates

A purpose-built screen for reviewing and acting on duplicate and near-duplicate
groups. Not a filter on the grid — the interaction model is fundamentally
different (groups, not individuals).

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  ← Gallery   Duplicates   [≡ exact/near/all] [threshold ▾]  │  ← toolbar
├──────────────────────────────────────────────────────────────┤
│  Group 1 of 847                              [Keep all]     │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐    │     │
│  │  │ KEEP ✓ │  │        │  │        │  │        │    │     │
│  │  │ 2.1 MB │  │ 2.1 MB │  │ 1.8 MB │  │ 1.6 MB │    │     │
│  │  │ 📍 orig │  │ copy/  │  │ resized│  │ edited │    │     │
│  │  │        │  │ Downloads│ │        │  │        │    │     │
│  │  └────────┘  └────────┘  └────────┘  └────────┘    │     │
│  │  [Keep marked]  [Remove marked → trash]             │     │
│  └─────────────────────────────────────────────────────┘     │
│                                                              │
│  Group 2 of 847                                              │
│  ┌─────────────────────────────────────────────────────┐     │
│  │  (near-duplicate group, similarity ~94%)            │     │
│  │  ┌────────┐  ┌────────┐  ┌────────┐                 │     │
│  │  │ KEEP   │  │        │  │        │                 │     │
│  │  └────────┘  └────────┘  └────────┘                 │     │
│  └─────────────────────────────────────────────────────┘     │
│  …                                                          │
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

**Group display:**
- Each group is a horizontal scrollable row of thumbnails.
- Exact duplicates (SHA256 match) are labeled "Exact" with identical hashes.
- Near duplicates (pHash Hamming distance) show the similarity percentage.
- The "best" candidate in each group is pre-selected as KEEP based on:
  largest file, sharpest (highest blur_score), and original filename (heuristic).
  The user can change which one is marked KEEP by clicking its badge.

**Actions per group:**
- **Keep marked** — removes the KEEP badge from the others, leaving them
  unchecked. Intended for "mark the one I want, delete the rest."
- **Keep all** — marks the whole group as "not duplicates after all" and
  dismisses it from the view. Does not delete anything; just removes the
  duplicates classification.
- **Trash selected** — trashes all non-KEEP items after confirmation.
  Equivalent to `phig rm` on those items.
- Individual thumbnails can be toggled (keep vs. remove) by clicking.

**Bulk actions** (toolbar):
- **Keep first in all groups** — marks the first (best) item as KEEP in every
  group. Opens a confirmation: "Mark 2,431 files for removal across 847 groups?"
- **Trash all marked** — after reviewing, executes the batch removal.

**Filters:**
- Type selector: Exact only, Near only, All.
- Threshold slider for near duplicates: 0–64 Hamming distance (default depends
  on phig's calibrated threshold).
- Match/exclude globs: filter which groups appear by path.

**Empty states:**
- **No duplicates found:** "No duplicates in this collection" with the type
  selector visible so the user can toggle between exact and near.
- **All groups reviewed:** "All duplicate groups reviewed" with a count of
  files marked for removal and a "Trash marked files" button.

### Platform notes

- **GTK:** `AdwNavigationPage` pushed from the sidebar. Groups in a
  `GtkListBox` with horizontal `GtkBox` rows of thumbnails.
- **macOS:** Detail view in the content area (sidebar remains visible).
  Groups in an `NSCollectionView` or `List` with horizontal scrolling rows.
- **Windows:** Sub-page with back button. Groups in a `ListView` or
  `ItemsRepeater` with a horizontal `ScrollView` per group. Bulk action buttons
  in a `CommandBar` at the top of the page.

---

## Screen 4: People / Faces

Browse photos by person, manage face clusters, and name unknown faces.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  ← Gallery   People                       [⚙ Face settings] │
├──────────────────────────────────────────────────────────────┤
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐        │
│  │   👤     │ │   👤     │ │   👤     │ │   👤     │        │
│  │   Mom    │ │   Dad    │ │  Sarah   │ │  ？？？  │        │
│  │  342 📷  │ │  218 📷  │ │   56 📷  │ │   89 📷  │        │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘        │
│  ┌──────────┐ ┌──────────┐                                   │
│  │   👤     │ │   👤     │   + [New person…]                 │
│  │  James   │ │  Unknown │                                   │
│  │   12 📷  │ │   23 📷  │                                   │
│  └──────────┘ └──────────┘                                   │
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

**Person grid:**
- Each named person is a card with a representative face thumbnail, name, and
  photo count.
- Unnamed clusters show as "Unknown" with a count.
- Click a person → filters the gallery to photos containing that person.

**Person detail** (click a person):
- Shows all photos of that person in the gallery grid.
- "Rename…" to correct a mislabeled person.
- "Merge with…" to combine two clusters that are the same person.
- "Not this person" on individual photos to remove the label.
- "Split" to separate a merged cluster.

**Face settings** (toolbar gear):
- "Detect faces in new photos" toggle (on by default for new scans).
- "Re-detect all faces…" — runs `phig face rebuild`, a job.
- Model download status.

### Relationship to gallery

People is both a sidebar browse target and a filter. Selecting "Mom" in the
sidebar switches the content area to the gallery grid filtered to photos
containing Mom. The person-detail view is the gallery with that filter applied
and a header showing the person's name and face.

---

## Screen 5: Organize (cp/mv)

A two-step wizard: configure, then review before executing.

### Step 1 — Configure

```
┌──────────────────────────────────────────────────────────────┐
│  Organize Photos                                             │
│                                                              │
│  Action:  (●) Copy    ( ) Move                               │
│                                                              │
│  Destination:  [/home/brett/Pictures/Sorted    ] [Browse…]   │
│                                                              │
│  Folder format:                                              │
│  ┌──────────────────────────────────────────────────────┐    │
│  │  %Y/%m/%d/%camera/%original                          │    │
│  │  Example: 2026/08/12/Canon EOS R5/IMG_4628.CR2       │    │
│  └──────────────────────────────────────────────────────┘    │
│  [▸] Presets:  Year/Month/Day  │  Year/Month  │  Custom     │
│                                                              │
│  Scope:  (●) All photos   ( ) Selected only (12 photos)     │
│          ( ) Filtered (matching current filters)             │
│                                                              │
│  On conflict:  (●) Skip   ( ) Overwrite   ( ) Rename        │
│                                                              │
│  [Cancel]                          [Preview →]               │
└──────────────────────────────────────────────────────────────┘
```

### Step 2 — Review

```
┌──────────────────────────────────────────────────────────────┐
│  Organize — Review                       12 files to copy    │
├──────────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Source                          → Destination          │  │
│  │  ────────────────────────────────────────────────────  │  │
│  │  ✓ /photos/2026/IMG_4628.CR2   → Sorted/2026/08/12/   │  │
│  │                                    Canon EOS R5/       │  │
│  │                                    IMG_4628.CR2        │  │
│  │  ✓ /photos/2026/IMG_4628.JPG   → (sidecar, moves      │  │
│  │                                    with RAW)           │  │
│  │  ✓ /photos/2026/IMG_4629.CR2   → Sorted/2026/08/12/   │  │
│  │                                    Canon EOS R5/       │  │
│  │                                    IMG_4629.CR2        │  │
│  │  …                                                    │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  [Back: Configure]     [× Cancel]     [Copy 12 files →]     │
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

- **Step 1** presents the same options as `phig cp`/`phig mv`'s flags.
- **Step 2** is the dry-run output rendered as a reviewable list. Each row has a
  checkbox; the user can uncheck individual files. Sidecars are shown indented
  beneath their parent.
- The final action button is labeled with the verb and count ("Copy 12 files").
- On completion: a toast shows the result and the Activity Log entry is
  created.

### Platform notes

- **GTK:** Step 1 as an `AdwPreferencesWindow`-style dialog or an
  `AdwNavigationPage`. Step 2 as a content page with a list.
- **macOS:** Step 1 as a sheet dialog. Step 2 as the same sheet transitioning
  to the review list, or a new window.
- **Windows:** Step 1 as a `ContentDialog`. Step 2 as a sub-page pushed onto
  the content frame, with a `CommandBar` at the bottom for Back/Cancel/Execute.

---

## Screen 6: Scan

A dialog for indexing directories into the collection.

```
┌──────────────────────────────────────────────────────────────┐
│  Scan Directory                                              │
│                                                              │
│  Directory:  [/home/brett/Pictures             ] [Browse…]   │
│                                                              │
│  [✓] Include subdirectories (recursive)                     │
│  [✓] Generate thumbnails                                    │
│  [ ] Detect faces                                            │
│                                                              │
│  What to scan:  (●) Images only  ( ) Videos only  ( ) Both  │
│  [ ] Include hidden files/directories                        │
│                                                              │
│  Re-scan policy:  (●) Skip unchanged  ( ) Force rehash      │
│                                          ( ) Force all       │
│                                                              │
│  [Cancel]                                [Scan →]           │
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

- On confirmation, the dialog closes and a job banner appears in the main window:
  "Scanning /home/brett/Pictures… 342 of 1,892 files (2 failures)"
- Failures are clickable; opens a detail list of files that couldn't be decoded.
- The gallery remains browsable while scanning (reads never block — WAL mode).
- On completion: toast with summary, activity log entry.

### Platform notes

- **GTK:** `AdwDialog` or `AdwPreferencesWindow` page.
- **macOS:** Sheet dialog or accessory view in an `NSOpenPanel`.
- **Windows:** `ContentDialog` with a directory picker. Could also integrate
  with the `FolderPicker` system dialog for directory selection.

---

## Screen 7: Activity Log (specialty screen)

The append-only audit trail of every file-changing operation. Not in the main
sidebar section — in the Actions group, slightly lower prominence.

### Layout

```
┌──────────────────────────────────────────────────────────────┐
│  ← Back   Activity Log                                       │
├──────────────────────────────────────────────────────────────┤
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Operation                    Date          Files       │  │
│  │  ────────────────────────────────────────────────────  │  │
│  │  📦 Copy to Sorted/         Aug 12 14:32   12 files   ▸│  │
│  │  🗑 Trash duplicates        Aug 11 09:15   47 files   ▸│  │
│  │  🔄 Move to Archive/        Aug 10 16:02   340 files  ▸│  │
│  │  📷 Scan ~/Pictures         Aug 10 15:58   1,892 new  ▸│  │
│  │  🔄 Move to Sorted/         Aug  9 11:20   56 files   ▸│  │
│  │  …                                                      │  │
│  └────────────────────────────────────────────────────────┘  │
│                                                              │
│  [Export log…]                    [Prune entries…]           │
└──────────────────────────────────────────────────────────────┘
```

### Behaviors

- Paginated list of operations, newest first. Each row shows the operation
  kind (icon), description, date, and file count.
- Click an operation expands it to show per-file entries with old and new paths.
- **Export log…** writes the full log as CSV/JSON.
- **Prune entries…** trims to the configured `max_entries`.

### Future: undo

When `phig undo` is implemented, a "Rollback this operation" button appears on
expanded operations where rollback is possible (no conflicting later operations).

---

## Screen 8: Jobs & Progress

Long-running operations report progress through a non-modal mechanism.

### GTK

- **`AdwBanner`** at the top of the content area: "Scanning ~/Pictures — 342/1,892"
  with a progress bar and a cancel button.
- Multiple concurrent jobs stack as multiple banners.
- A progress popover in the header bar (clock/sync icon) lists all running and
  recent jobs.
- Completed jobs show a toast: "Scan complete: 1,892 files indexed, 2 failed."
  The failure count is clickable for details.

### macOS

- A progress indicator in the toolbar (spinning indicator or progress bar).
- Clicking it opens a popover with running and recent jobs.
- Completed jobs post a notification via UserNotifications (if permitted).

### Windows

- **`InfoBar`** at the top of the content area: "Scanning ~/Pictures — 342/1,892"
  with a `ProgressBar` and a Cancel button.
- Multiple concurrent jobs stack as multiple `InfoBar` elements.
- A `ProgressRing` or clock icon in the title bar opens a flyout with running
  and recent jobs.
- Completed jobs: `InfoBar` transitions to a success state with a summary;
  optionally posts a toast notification via Windows App SDK notifications.
- Jobs appear in the Windows share/notification center if the app registers a
  background task (future).

### Job types with progress

| Operation | Progress granularity | Cancel behavior |
|---|---|---|
| Scan | Per file processed | Stops after current file; partial results committed |
| Thumbnails rebuild | Per file | Stops after current; partial results committed |
| Face detection | Per file | Stops after current; partial results committed |
| Face identify | Per file | Stops after current |
| Organize (cp/mv) | Per file copied/moved | Stops after current; partial results committed |
| Remove (rm) | Per file | Stops after current; partial results committed |

---

## Screen 9: Settings / Preferences

Platform-native preferences window.

### General

- **Database location:** path to `phig.db` (with a Browse/"…" button).
  On Windows, defaults to `%LOCALAPPDATA%\phig\phig.db`.
- **Default scan directory:** for the Scan dialog's initial directory.
- **Scan defaults:** thumbnails on/off, faces on/off, recursive on/off, media
  type, re-scan policy.
- **Organize defaults:** copy vs. move, destination, format string, conflict
  policy.

### Thumbnails

- **Cache location:** defaults to XDG cache (Linux), `~/Library/Caches/phig/thumbs` (macOS),
  or `%LOCALAPPDATA%\phig\thumbs` (Windows).
- **Default thumbnail size:** 512px (the size `scan --thumbs` generates).
- **Cache usage:** current size on disk with a "Clear cache" button.
- **Max cache size:** optional cap with LRU eviction (future).

### Activity Log

- **Enable logging:** toggle.
- **Max entries:** entry count cap (0 = unlimited).

### Faces

- **Face detection enabled:** toggle for new scans.
- **Model status:** whether detection and recognition models are downloaded.
- **Download models:** button if not present.
- **Minimum face size:** pixels (default from phig).

---

## Context Menus

Right-click on a thumbnail (grid or viewer):

```
┌──────────────────────────┐
│  Open in Viewer          │
│  Open in System Viewer   │
│  Show in File Manager    │
│  ─────────────────────── │
│  ☆ Favorite              │
│  Tag…                    │
│  Add to Gallery…         │
│  ─────────────────────── │
│  Copy Path               │
│  ─────────────────────── │
│  Move to Trash…          │
│  Delete Permanently…     │
│  Remove from Database…   │
└──────────────────────────┘
```

On macOS, the Finder-level context menu (right-click in Finder) could gain a
"Show in Phig" service — out of scope for initial release but worth noting.

On Windows, phig could register as a shell extension for image file types,
adding "Open in Phig" / "Show in Phig" to the Explorer context menu and
providing thumbnail previews in File Explorer from phig's thumbnail cache.
Out of scope for initial release.

---

## Keyboard Shortcuts

### Common (all platforms)

| Key | Action |
|---|---|
| `←` `→` | Previous/next image (viewer) |
| `↑` `↓` | Navigate grid |
| `Enter` | Open selected image in viewer |
| `Space` | Quick Look / toggle viewer (macOS); open viewer (GTK) |
| `Ctrl+F` / `Cmd+F` | Focus search field |
| `Ctrl+Z` / `Cmd+Z` | Undo last tag/favorite/gallery change |
| `Delete` / `Backspace` | Move selected to trash (with confirmation) |
| `Escape` | Close viewer / clear selection / dismiss dialog |

### GTK-specific

| Key | Action |
|---|---|
| `Ctrl+Q` | Quit |
| `Ctrl+,` | Settings |
| `Ctrl+scroll` / `Ctrl++/-` | Zoom grid |
| `F9` | Toggle sidebar |
| `Ctrl+T` | Tag selected |
| `Ctrl+G` | Gallery selected |

### macOS-specific

| Key | Action |
|---|---|
| `Cmd+Q` | Quit |
| `Cmd+,` | Preferences |
| `Cmd+scroll` / `Cmd++/-` | Zoom grid |
| `Cmd+Option+S` | Toggle sidebar |
| `Cmd+Option+I` | Toggle inspector |
| `Cmd+T` | Tag selected |
| `Cmd+Shift+G` | Gallery selected |
| `Space` | Quick Look (system-provided, in the grid) |

### Windows-specific

| Key | Action |
|---|---|
| `Ctrl+Q` | Quit |
| `Ctrl+,` | Settings |
| `Ctrl+scroll` / `Ctrl++/-` | Zoom grid |
| `Ctrl+B` | Toggle sidebar (hamburger) |
| `Ctrl+T` | Tag selected |
| `Ctrl+G` | Gallery selected |
| `Backspace` | Back (from viewer to gallery) |
| `Alt+←` / `Alt+→` | Back/forward navigation |
| `XButton1` / `XButton2` | Back/forward (browser mouse buttons) |

---

## First-Launch Experience

1. App opens to the gallery grid with an empty-state page: "No photos yet."
2. A prominent button: "Scan a folder…" — opens the Scan dialog.
3. Below it, a secondary link: "Open an existing database…" — for users who
   already have a phig collection from the CLI.
4. While the first scan runs, the grid populates as files are indexed.
5. On scan completion: a toast "1,892 photos indexed" and the gallery is
   immediately browsable.

---

## Screen inventory summary

| Screen | Type | How reached | Purpose |
|---|---|---|---|
| Gallery | Primary | Default / sidebar "All Photos" | Browse, filter, select |
| Viewer | Secondary | Click thumbnail in gallery | Single-image inspection |
| Duplicates | Primary | Sidebar "Duplicates" | Review and resolve duplicate groups |
| People | Primary | Sidebar "People" | Browse and manage face clusters |
| Scan | Dialog | Sidebar "Scan…" | Index a directory |
| Organize | Wizard | Sidebar "Organize…" / selection bar | cp/mv files with dry-run review |
| Remove | Dialog | Context menu / selection bar / duplicates screen | Trash/hard-delete/db-only |
| Activity Log | Specialty | Sidebar "Activity Log" | Audit trail of past operations |
| Settings | Dialog | Sidebar / app menu | Preferences |
| Job Progress | Overlay | Automatic during operations | Non-modal progress + cancellation |

---

## Platform idiom summary

| Concern | GTK (GNOME) | macOS | Windows |
|---|---|---|---|
| Framework | GTK4 + libadwaita, C | SwiftUI (preferred) or AppKit, Swift | WinUI 3 + Windows App SDK, C++/C# |
| Navigation | `AdwNavigationSplitView` + `AdwNavigationPage` push/pop stack | `NavigationSplitView` three-column; `NavigationLink` push | `NavigationView` with hamburger pane; `Frame.Navigate` for sub-pages; back button in title bar |
| Sidebar | Flat list with section headers; expandable rows | Source list (`SidebarListStyle`) with disclosure triangles | Navigation menu with `NavigationViewItem` + `NavigationViewItemHeader`; `IconSource` per item |
| Header | `AdwHeaderBar` with pack-start/pack-end children, search entry, buttons | Unified toolbar with `NSToolbar`, centered search field, trailing controls | Title bar with `AutoSuggestBox` (search), back button when navigated; `CommandBar` or toolbar row |
| Inspector | Bottom sheet or popover on selection (not a permanent column) | Third column in split view; `Inspector` pane | Right-hand flyout or bottom sheet; `Expander` in narrow layouts |
| Tabs | Not used (navigation stack instead) | Optional tabbed browsing within content area | `TabView` within content area if needed (e.g., multiple viewers) |
| Dialogs | `AdwDialog`, `AdwPreferencesWindow` pages | Sheets (`NSWindow.sheet`), popovers, `Settings` scene | `ContentDialog` for confirmations and wizards; `ContentDialog` with pages for multi-step |
| Toasts | `AdwToast` overlay from bottom | Notification banner or UserNotifications | `InfoBar` inline (success/warning/error); `TeachingTip` for transient callouts; Windows App SDK toast notifications |
| Progress | `AdwBanner` in content area, progress popover in header bar | Toolbar progress indicator, popover | `InfoBar` with `ProgressBar`; title bar `ProgressRing` + flyout; `StatusBar` at bottom |
| Context menu | Right-click `GtkPopoverMenu` | Right-click `NSMenu` | Right-click `MenuFlyout` or `CommandBarFlyout` |
| Zoom | Pinch gesture + Ctrl+scroll + toolbar +/- | Pinch gesture + Cmd+scroll + toolbar segmented control | Pinch gesture (precision touchpad) + Ctrl+scroll + `CommandBar` +/- buttons |
| Shortcuts | GNOME HIG standard accelerators (`Ctrl+`) | macOS standard key bindings (`Cmd+`) | Windows standard accelerators (`Ctrl+`); back via `Backspace` / `Alt+←` / XButton1 |
| Window title | Application name ("phig") | Display name in title bar, no title on window | Application name in title bar; back button appears when navigated |
| Menu bar | Not used (header bar actions, primary menu) | Standard `NSMenu` bar (File, Edit, View, Window, Help) | Not used (title bar + `NavigationView`; standard File/Edit/etc. are optional, actions live in `CommandBar`) |
| Preferences | `AdwPreferencesWindow` | `Settings` scene (`Cmd+,`) | Settings page within `NavigationView` (Settings-style pattern) or a separate `ContentDialog` |
| Selection | Click + Ctrl+click range, Shift+click range | Click + Cmd+click disjoint, Shift+click range | Click + Ctrl+click (disjoint), Shift+click (range) — Windows convention |
| Adaptive | `AdwBreakpoint` collapses sidebar at narrow widths | Sidebar toggle button; `TabView` on compact sizes | `NavigationView` auto-collapses to hamburger at narrow widths; `PaneDisplayMode` adapts |
| Title bar style | CSD (client-side decorations), dark/light via libadwaita | Native title bar; translucent/sidebar tint via SwiftUI | Mica/acrylic title bar via WinUI `Window.SystemBackdrop`; supports snap layouts natively |
| Open with | `.desktop` file + MIME type registration | `CFBundleDocumentTypes` + `LSCopyDefaultApplicationURL` | Shell extension + registered file type associations (HKEY_CLASSES_ROOT) |

---

## Dependencies

- **031** — libphig operations surface (scan, thumbs, faces as ABI primitives
  with progress)
- **039** — cross-platform core/libphig (lockfile, paths, case-insensitive
  filesystems — unblocks the Windows client)
- **041** — scan reconcile plan/apply (the dry-run-then-confirm pattern the
  Organize and Scan dialogs depend on)
- **042** — organize engine (cp/mv as ABI entry points with progress)
- **043** — CLI primitive split (validates that scan/thumbs/faces are separable
  operations the GUI can call independently)

## Open questions

1. **Framework finalization for Windows.** WinUI 3 is the design target; WPF is
   the fallback if WinUI 3's C++ story proves too immature. Decision after a
   spike that exercises libphig interop from both.
2. **macOS framework.** SwiftUI is preferred; AppKit may be necessary for
   low-level image rendering performance in the viewer. The virtualized grid in
   M0 is already proven in GTK — the rendering path is the unknown on macOS.
3. **phig-decode for macOS/Windows.** The design spec calls for an out-of-process
   decode worker pool (crash isolation). This exists as a design but not as a
   shipped component on any platform. Is it buildable on macOS (sandboxing,
   `memfd_create` equivalent) and Windows (`CreateFileMapping`/`SEC_SIMAGE`)?
4. **Writeback of tags to XMP/IPTC.** Repeatedly requested in the competitive
   analysis (MARKET.md) — Lap users want it, DAM buyers expect it. phig stores
   tags only in `phig.db`. Should the GUI write tags back to XMP sidecars, and
   if so, is that a core/libphig capability or GUI-only? This is a product
   decision, not a widget question.
5. **Star ratings.** Culling demand appears in three of six MARKET.md threads,
   and the mechanism users describe is 1–5 star ratings with filter-by-rating —
   not phig's binary favorite. Does phig add a rating scale, or does the GUI
   map "favorite" to a culling workflow?

## What this design defers

- **Immich/sync integration.** Immich is a competitor and Lap already occupies
  the "desktop client for Immich" niche. Not in scope until there is user demand.
- **Built-in photo editor.** Explicitly excluded — phig is an organizer.
  System viewer handles display; external editor for adjustments.
- **Multi-collection / server mode.** Local single-user only.
- **Mobile.** The libphig C ABI exists for a future Elixir web client (per the
  root CLAUDE.md), but this design covers desktop only.
- **P2P/sync.** Not in the product scope; the MARKET.md analysis found no
  primary-source demand.
- **Natural-language search.** Lap does this via CLIP embeddings. phig has
  pHash — better for duplicate detection, cannot do semantic search. A future
  CLIP integration is a separate plan.

## Cross-references

- Root `CLAUDE.md` — phig's feature surface and architecture.
- `docs/specs/2026-07-31-phig-gtk-design.md` — the M0-era design spec, which
  this document extends to cover the full feature surface and all three platforms.
- `plans/035-gtk-m0.md` — what shipped in M0.
- `plans/031-libphig-operations.md` — the ABI surface for scan/thumbs/faces
  operations that jobs need.
- `plans/039-core-cross-platform.md` — cross-platform engine work (macOS and
  Windows paths, lockfiles, case-insensitive filesystems).
- `plans/041-scan-reconcile-plan-apply.md` — the plan/apply split that the
  Scan dialog and Organize review step depend on.
- `plans/043-cli-primitives.md` — breaks `scan --thumbs --faces` into
  callable primitives, which the GUI jobs depend on.
- `MARKET.md` — competitive analysis behind this design's priorities.

The Windows client specifically depends on plan 039 (cross-platform `core/` and
`libphig/`), which covers the `flock` → `LockFileEx` change, path comparison on
case-insensitive NTFS, and `%LOCALAPPDATA%` data-directory resolution. WinUI 3
is the natural framework choice: it is the current Microsoft-backed native
Windows UI stack, ships with Windows App SDK, supports C++/C# interop (phig's
libphig is a C ABI callable from either), and delivers the Mica/acrylic design
language users expect on Windows 11. WPF is a fallback if WinUI 3's C++ story
proves too immature — it has a stable C++/CLI interop path but cannot match the
modern look.
