---
name: Face Recognition
status: planned
created: 2026-03-28
description: >
  Detect and recognize faces in images using dlib. Store face embeddings in
  SQLite with sqlite-vec for fast vector similarity search. Search by providing
  a reference image of a person.
---

# Face Recognition

## Goal

Detect faces in scanned images, compute 128D face embeddings, and store them in the database. Enable searching for all images containing a specific person by providing a reference photo.

## Libraries

- **dlib** — face detection (HOG or CNN) + 128D face embedding (ResNet). Dynamic linking via nixpkgs.
- **sqlite-vec** — zero-dependency SQLite extension for vector search. Vendored single .c/.h file.

### dlib Models (pre-trained, freely available)

- `shape_predictor_68_face_landmarks.dat` — face landmark detection
- `dlib_face_recognition_resnet_model_v1.dat` — 128D embedding model

Models would be downloaded on first use or bundled. Need to decide on a model storage location (e.g., `~/.local/share/phig/models/`).

## Schema

```sql
CREATE TABLE faces (
    id          INTEGER PRIMARY KEY,
    image_id    INTEGER NOT NULL REFERENCES images(id),
    embedding   BLOB NOT NULL,     -- 128 x float32 = 512 bytes
    x           INTEGER,           -- bounding box top-left x
    y           INTEGER,           -- bounding box top-left y
    width       INTEGER,           -- bounding box width
    height      INTEGER            -- bounding box height
);

-- sqlite-vec virtual table for fast KNN search
CREATE VIRTUAL TABLE vec_faces USING vec0(
    face_id INTEGER PRIMARY KEY,
    embedding float[128]
);
```

The `vec_faces` table mirrors `faces.embedding` and enables:
```sql
SELECT f.image_id, i.path, v.distance
FROM vec_faces v
JOIN faces f ON f.id = v.face_id
JOIN images i ON i.id = f.image_id
WHERE v.embedding MATCH ?     -- query embedding
ORDER BY v.distance
LIMIT 20;
```

## CLI

### Scanning (face detection during scan)

```
phig scan <directory> [--faces] [flags]
```

`--faces` enables face detection + embedding during the scan pipeline. It's smart about what work to do:
- **New files (not in DB):** Full pipeline — hash + phash + EXIF + face detection
- **Existing files without face data:** Run face detection only, skip hash/phash/EXIF
- **Existing files with face data:** Skip entirely (unless `--force`)

Without `--faces`, scan works as before (no dlib dependency at runtime for users who don't need face features).

### Searching by face

```
phig search --face <image-path> [--threshold <float>] [--limit N] [flags]
```

1. Detect faces in the provided image
2. Compute embedding for each face found
3. If multiple faces detected, prompt or use `--face-index N` to select which
4. Query `vec_faces` for nearest neighbors
5. Return matching images sorted by similarity

## Pipeline

### During scan (with `--faces`)

```
image → dlib face detector → 0..N face bounding boxes
  → for each face:
      → dlib landmark predictor → 68 landmarks
      → dlib face encoder → 128D embedding (float32)
      → insert into faces table + vec_faces table
```

### During search

```
query image → same pipeline → query embedding
  → SELECT from vec_faces WHERE embedding MATCH query
  → join back to images table for paths
```

## Embedding Comparison

dlib embeddings use Euclidean distance. Typical thresholds:
- < 0.6: same person (dlib's recommended threshold)
- 0.6 - 0.8: possibly same person
- > 0.8: different people

sqlite-vec supports Euclidean distance (L2) natively.

## Open Questions

- **Model management:** Download on first use, bundle with binary, or expect user to provide? Where to store? (`~/.local/share/phig/models/`)
- **`--faces` as default?** Should face detection be opt-in per scan, or always-on once models are present? Currently opt-in.
- **Named people:** Future feature — `phig tag-face <image> <name>` to label a face cluster. The faces table could get a `person_id` column linking to a `people` table. Not in scope for this plan.
- **Performance:** dlib CNN face detector is slow (~1-2s per image on CPU). HOG detector is faster (~50ms) but less accurate. Should we default to HOG and offer `--face-model hog|cnn`?
- **Multiple faces in query image:** If the reference photo has multiple faces, how to select? `--face-index N` with a preview? Or detect the largest face by default?
- **sqlite-vec integration:** Vendor the .c/.h file, or use CMake FetchContent?

## Checklist

- [ ] Add dlib to devenv.nix
- [ ] Vendor or fetch sqlite-vec
- [ ] Face detection module (`src/faces.h/cpp`)
- [ ] Face embedding computation via dlib
- [ ] `faces` table schema + migration
- [ ] `vec_faces` virtual table setup
- [ ] `--faces` flag on scan command
- [ ] Insert embeddings into both tables during scan
- [ ] `phig search --face <image>` command
- [ ] KNN query via sqlite-vec
- [ ] Model download / management
- [ ] Tests for face detection, embedding storage, vector search
