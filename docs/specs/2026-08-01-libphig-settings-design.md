# libphig settings API — Design

Status: draft
Created: 2026-08-01
Follows: [phig-gtk design](2026-07-31-phig-gtk-design.md); libphig core (phig
schema v7, ABI 1.0, merged 2026-08-01)

## What this is

A settings API for `libphig`: read phig's config file, override values, write
changes back preserving the user's formatting. It closes a gap the core ABI
left — a client currently has to parse `config.ini` itself to learn where the
user's database is, so the GTK app and the CLI could disagree about which
collection is "yours".

## Requirements

1. **A client gets the same defaults the CLI uses**, in one call, without
   reimplementing phig's config parsing.
2. **No hidden filesystem access in `phig_init`.** Loading config is an explicit,
   separate call. `phig_init` stays a SQLite open, which the GTK app's
   time-to-first-window budget depends on.
3. **New phig settings reach clients without an ABI change.** Adding a field to
   a struct is a layout change and a major version bump; the config file will
   keep growing.
4. **Writing back preserves the file.** Comments, ordering, and keys libphig
   does not model must survive a GUI save.
5. **Precedence is the caller's, and visible in their code:** config file, then
   app settings, then command-line flags.

## Design decisions

**Generic key/value, not typed struct fields.** `phig_settings` is an opaque
handle over `section.name` → string pairs. phig can add `[scan] foo` and every
existing client reads it without recompiling. A GTK settings dialog can
enumerate keys and render an editor without hardcoding a schema.

The tradeoff is losing compile-time checking:
`phig_settings_get_int(s, "general.parallell", 4)` silently returns the
fallback. Mitigated with `PHIG_KEY_*` constants for documented keys — clients
get autocomplete and typo-resistance, while arbitrary string keys stay available
for anything phig adds later.

**`phig_config` keeps its typed fields.** `db_path`, `thumbs_dir`, and `mode`
are consumed by `phig_init` itself, so they stay explicit in the signature — a
caller can see what the library requires. `phig_settings_to_config` bridges the
two. Making `phig_init` take a settings handle instead would hide its required
inputs behind string lookups.

**`mode` never comes from the config file.** It is a per-context decision (the
GUI opens read-only for queries, read-write for tag edits), not a user
preference. `phig_settings_to_config` leaves it untouched.

**INI stays; the format does not change.** TOML was considered and rejected.
phig's config is flat `section.name = scalar`, which is the subset where INI and
TOML are near-identical. The writer has to be hand-written either way
(`inih` is parse-only), and for flat INI that is a small amount of code, whereas
a format-preserving TOML writer means taking a large dependency — against plan
018's stock-system-libraries goal. The generic accessor also makes the on-disk
format less relevant, since callers never see it. Revisit only if phig gains
genuinely structured config (a watch list, per-collection sub-tables, arrays of
globs), and revisit *before* shipping such a setting rather than after.

## API

Added to `include/phig/phig.h`. ABI minor bump (additive only; no existing
struct layout or enum value changes).

### Handle and lifecycle

```c
typedef struct phig_settings phig_settings;

/* Loads phig's config file: `path` when non-NULL, else the default
   (~/.config/phig/config.ini, honoring XDG_CONFIG_HOME).

   Returns PHIG_ERR_NOT_FOUND when no file exists — *out still receives a valid
   empty handle, so a caller wanting defaults can ignore that status, and a
   caller creating a config can populate and save it. Returns PHIG_ERR_DB on a
   malformed file, with *out set to NULL. */
PHIG_API phig_status phig_settings_load(const char *path, phig_settings **out);

/* Safe to call with NULL. */
PHIG_API void phig_settings_free(phig_settings *s);
```

### Reading

Keys are `"section.name"`. Lookups are case-sensitive, matching phig's parser.

```c
/* Returns `fallback` when the key is absent. The returned pointer is owned by
   `s` and is valid until the key is set/unset or `s` is freed. */
PHIG_API const char *phig_settings_get(phig_settings *s, const char *key,
                                       const char *fallback);

/* Return `fallback` when the key is absent OR its value does not parse as the
   requested type. Bools accept true/false/yes/no/on/off/1/0, case-insensitive —
   the same forms phig's config parser accepts. */
PHIG_API int64_t phig_settings_get_int(phig_settings *s, const char *key,
                                       int64_t fallback);
PHIG_API bool    phig_settings_get_bool(phig_settings *s, const char *key,
                                        bool fallback);

PHIG_API bool phig_settings_has(phig_settings *s, const char *key);
```

Path values get `~` expanded on read, matching CLI behavior. The raw
(unexpanded) value is what `phig_settings_save` writes back, so a config saying
`~/photos` stays `~/photos` after a GUI save that did not touch it.

### Enumeration

How a GUI builds a settings editor without a compiled-in schema.

```c
PHIG_API size_t      phig_settings_count(phig_settings *s);
/* Key at index i in file order, or NULL if i is out of range. */
PHIG_API const char *phig_settings_key_at(phig_settings *s, size_t i);
```

Indices are stable until the next `set`/`unset`.

### Writing

```c
/* Sets or replaces a key. A key phig does not know is allowed — clients may
   store their own, and unknown keys already in the file are preserved. */
PHIG_API phig_status phig_settings_set(phig_settings *s, const char *key,
                                       const char *value);
PHIG_API phig_status phig_settings_unset(phig_settings *s, const char *key);

/* Writes to `path`, or the loaded/default path when NULL. Atomic: writes a
   temp sibling and renames, so an interrupted save cannot truncate the file. */
PHIG_API phig_status phig_settings_save(phig_settings *s, const char *path);
```

### Bridging to `phig_config`

```c
/* Fills cfg->db_path/thumbs_dir/models_dir from settings, leaving fields the
   config does not set untouched so a caller can pre-seed defaults. Does NOT
   set cfg->mode — that is a per-context decision, not a user preference.

   The strings point into `s` and are valid until it is freed, so `s` must
   outlive the phig_init call. */
PHIG_API phig_status phig_settings_to_config(phig_settings *s, phig_config *cfg);
```

### Known-key constants

```c
#define PHIG_KEY_DB           "paths.db"
#define PHIG_KEY_THUMBS_DIR   "paths.thumbs_dir"
#define PHIG_KEY_MODELS_DIR   "paths.models_dir"
#define PHIG_KEY_PARALLEL     "general.parallel"
#define PHIG_KEY_LOG_ENABLED  "log.enabled"
#define PHIG_KEY_LOG_MAX      "log.max_entries"
#define PHIG_KEY_LOG_FILE     "log.file"
#define PHIG_KEY_LOG_LEVEL    "log.file_level"
```

Documented keys only. Per-command defaults (`[scan]`, `[organize]`,
`[duplicates]`, `[faces]`) are readable through the generic accessor but get no
constants — they configure CLI invocations, not a library context, and blessing
them in the header implies a support level libphig does not provide.

## The writer

The requirement is that a hand-edited config survives a GUI save.

**Model.** Parse into an ordered list of lines, each tagged as section header,
key/value (with its section, key, raw value, and byte offsets), comment, or
blank. Reads resolve against an index over the key/value lines; the line list is
what gets written back.

**On save:**
- A changed key rewrites its existing line in place, preserving surrounding
  whitespace and any trailing comment on that line.
- A new key is appended to the end of its section, creating the section at the
  end of the file if absent.
- An unset key removes its line only.
- Every other line — comments, blanks, unknown keys, ordering — is byte-identical.

**Atomicity.** Write to a temp sibling in the same directory, `fsync`, then
`rename`. phig already has `temp_sibling()` in `src/paths.h` for exactly this;
reuse it rather than adding a second mechanism.

**Concurrency.** Two writers racing (a GUI settings dialog and a CLI invocation)
is possible but out of scope: phig's CLI never writes the config file today, so
the only writer is the client. The atomic rename means a race loses one edit
rather than corrupting the file. If phig later gains config-writing commands,
this needs the advisory lock.

## Implementation notes

Parsing lives in a new `src/settings.cpp` / `src/settings.h` in phig's core, not
in `src/libphig/` — it is phig functionality that the CLI could also adopt, and
the ABI layer stays a thin wrapper. The existing `src/config.cpp` (which loads
`Config` via `inih`) is left alone: it and the new module read the same file for
different purposes, and merging them is a larger refactor than this design
needs. That duplication is a known cost, noted here so it is deliberate rather
than accidental.

`inih` stays the parser for `config.cpp`. The new module needs line positions
for the preserving writer, which `inih`'s callback interface does not provide,
so it does its own line-oriented parse — flat `key = value` with `[section]`
headers, `#` and `;` comments.

## Testing

| Layer | Approach |
|---|---|
| Parser | Real config files: comments in every position, `#` and `;`, whitespace variants, duplicate keys, keys before any section, CRLF, missing trailing newline, empty file. |
| Preserving writer | Round-trip a hand-written file with comments and unknown keys through load → set one key → save, and assert every other byte is unchanged. This is the requirement; test it directly rather than by inspection. |
| Atomicity | Save over an existing file and assert no temp sibling remains; assert the original is intact if the write fails. |
| ABI | Load-missing-file returns NOT_FOUND with a usable empty handle; get/get_int/get_bool fallbacks; enumeration order and index stability; NULL-argument rejection on every entry point; string lifetime across set/unset. |
| Bridge | `phig_settings_to_config` fills paths, leaves `mode` alone, and leaves unset fields untouched. |

Per the project's standing rule, these ship with real-behavior tests, not mocks.

## Out of scope

- Migrating `src/config.cpp` onto the new parser.
- CLI commands for editing config.
- Validation of values against phig's rules (`media` must be image|video|all,
  etc.) — the CLI validates on load, and duplicating that here would let the two
  drift. A GUI wanting validation should surface phig's error on next use.
- Structured config (arrays, nested tables) and any format change.
