---
name: Broader Image Format Support
status: deferred
created: 2026-08-08
description: >
  The formats plan 048 pruned from `known_extensions()` rather than fixed —
  psd, xcf, svg, ico — plus whatever else turns out to be advertised and
  undecodable. Each needs a different library and none blocks anything, so they
  are grouped here rather than widening 048. Deferred until a real collection
  contains files phig skips.
---

# 051 — Broader Image Format Support

## Why this exists

Plan 048 measured which of `known_extensions()`'s 24 advertised image formats
phig can actually decode. It fixed the four that mattered — heic, heif, avif,
jxl — and pruned four it could not justify carrying:

| Format | What it would need |
|---|---|
| `psd` | Layered Photoshop parsing. OpenCV rejects it (measured). |
| `xcf` | GIMP's native format; no mainstream C/C++ reader. |
| `svg` | Vector rasterization (resvg, librsvg) — a rendering decision, not a decode. |
| `ico` | Multi-resolution container; needs a size-selection rule. |

Pruning means scan skips them silently, the way it already skips `.tga`, rather
than reporting one decode error per file for a format nothing links a codec
for. The list now describes what phig can do.

## Why deferred

None of these blocks anything. None appears in a Photos library. Each brings a
separate dependency, and `svg` in particular is not really a decode question —
rasterizing a vector image at some chosen resolution to compute a perceptual
hash is a design decision about what the phash of a vector drawing even means.

The trigger for picking this up is evidence: a real collection containing files
phig skips, and a user who wants them indexed.

## When picked up

- Re-run 048's coverage measurement first. The answer changes as system OpenCV
  and ImageIO change — ImageIO already reads 62 types on macOS 26, and some of
  these may come free there.
- Decide per format whether it is worth a dependency, rather than adopting the
  group.
- `svg` needs its own decision about rasterization size and what a perceptual
  hash of a vector image should mean.

## Cross-references

- `plans/048-modern-image-codecs.md` — pruned these; holds the coverage
  measurement and the `core/image_codec` module they would extend.
