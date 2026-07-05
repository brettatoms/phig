---
name: Favorites
status: superseded
created: 2026-04-01
superseded_by: 011-tags-galleries.md
description: >
  Superseded by plan 011. Favorites are implemented as a protected built-in
  `favorite` tag with `phig fav` convenience commands, not a standalone boolean
  column — one annotation system shared with tags.
---

# Favorites (superseded)

**This plan is superseded by [011-tags-galleries.md](011-tags-galleries.md).**

The original design added a standalone boolean `favorite` column plus its own
`set_favorite`/`get_favorites` methods. That duplicated the annotation machinery
introduced by the tags system, so favorites were folded into plan 011:

- `favorite` is a normal tag, seeded at schema init and protected from deletion,
  so `--fav`/`%fav` always have a stable referent.
- `phig fav add/remove/list` are convenience sugar over the `favorite` tag
  (`phig fav add X` ≡ `phig tag add favorite X`).
- `phig search --fav` ≡ `--tag favorite`; `%fav` organize token resolves to
  `favorites` / `unfavorited`; `phig duplicates --fav` filters to groups with a
  favorite.

See plan 011 for the full design, CLI, and checklist.
