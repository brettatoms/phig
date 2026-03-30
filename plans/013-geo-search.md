---
name: Geolocation Search
status: planned
created: 2026-03-29
description: >
  Offline geolocation search using SpatiaLite and Natural Earth boundary data.
  Parse GPS EXIF coordinates at scan time, store as SpatiaLite geometry points,
  reverse-geocode to country/region using polygon containment, and expose
  location-based search via --location and --near flags.
---

# Geolocation Search

## Goal

Search images by where they were taken. Fully offline — no network calls. Uses SpatiaLite (spatial extension for SQLite) with Natural Earth boundary polygons for reverse geocoding. Supports both name-based search (`--location Belize`) and coordinate-based search (`--near 17.2,-88.5 --radius 50`).

## Dependencies

- **SpatiaLite** (`libspatialite`) — spatial extension for SQLite. Provides geometry types, spatial indexes (R*Tree), and spatial functions (`ST_Within`, `ST_Distance`, `MakePoint`, etc.). Available in nixpkgs as `libspatialite`.
- **Natural Earth data** — free public-domain vector datasets:
  - **Admin-0 countries** (1:110m, ~800KB) — country boundaries
  - **Admin-1 states/provinces** (1:10m, ~15MB) — state/province boundaries
  - **Populated places** (1:10m, ~3MB) — major cities/towns with coordinates

SpatiaLite transitively pulls in GEOS and PROJ.

## Design

### Schema Changes

Add a `geo` table (or columns on `images`) to store parsed GPS data and resolved location names. Keep the existing `exif` JSON blob unchanged.

```sql
-- New columns on images table
ALTER TABLE images ADD COLUMN geo_lat REAL;        -- decimal latitude
ALTER TABLE images ADD COLUMN geo_lon REAL;        -- decimal longitude
ALTER TABLE images ADD COLUMN geo_country TEXT;     -- resolved country name
ALTER TABLE images ADD COLUMN geo_region TEXT;      -- resolved state/province
ALTER TABLE images ADD COLUMN geo_point BLOB;       -- SpatiaLite POINT geometry
```

Spatial index on `geo_point` via SpatiaLite's `CreateSpatialIndex()`.

### Reference Data

Natural Earth shapefiles are loaded into reference tables in the phig database on first use:

```sql
-- Populated by loading Natural Earth shapefiles
ne_countries   (name TEXT, iso_a2 TEXT, geometry POLYGON)
ne_admin1      (name TEXT, country TEXT, geometry POLYGON)
```

SpatiaLite can load shapefiles directly via `SHP_Read()` or we can use `spatialite_tool` / a one-time C API import. The reference tables get spatial indexes for fast containment queries.

**Data bundling strategy:** Ship the Natural Earth shapefiles as project data files installed alongside the binary (or download on first run to `~/.local/share/phig/geodata/`). The download-on-first-run approach avoids bloating the binary but requires a one-time network fetch. The ship-with-binary approach is simpler. Decide during implementation.

### GPS EXIF Parsing

libexif returns GPS data as strings:
- `GPSLatitude`: `"17, 15, 30.12"` (degrees, minutes, seconds)
- `GPSLatitudeRef`: `"N"` or `"S"`
- `GPSLongitude`: `"88, 30, 0.00"`
- `GPSLongitudeRef`: `"E"` or `"W"`

Add a parsing function:

```cpp
// Returns nullopt if GPS tags are missing or malformed
struct GeoCoord { double lat; double lon; };
std::optional<GeoCoord> parse_gps_exif(const std::string& exif_json);
```

Conversion: `decimal = degrees + minutes/60 + seconds/3600`, negated if ref is `S` or `W`.

### Scan-Time Geocoding

During `phig scan`, after extracting EXIF:

1. Parse GPS EXIF → `(lat, lon)` decimal coordinates
2. Store `geo_lat`, `geo_lon`, and `geo_point` (SpatiaLite POINT) on the image row
3. Reverse geocode via spatial query:
   ```sql
   SELECT name FROM ne_countries
   WHERE ST_Within(MakePoint(?, ?, 4326), geometry)
   LIMIT 1
   ```
4. Store resolved `geo_country` and `geo_region` on the image row

Images without GPS EXIF get NULLs for all geo columns.

Re-scan behavior: geo columns are recomputed when an image is re-processed (same as other EXIF data). The `--force` flag triggers reprocessing.

### SpatiaLite Initialization

On database open:
1. Load the SpatiaLite extension (`mod_spatialite`)
2. If reference tables don't exist, load Natural Earth shapefiles and create spatial indexes
3. Run schema migration to add geo columns if missing

Keep SpatiaLite loading behind a lazy init — if no geo features are used and shapefiles aren't installed yet, don't block non-geo operations.

## CLI Changes

### Search flags (extends `phig search`)

```
--location <string>     Substring match on country or region name (e.g., "Belize", "California")
--near <lat,lon>        Find images near a GPS coordinate (decimal degrees)
--radius <km>           Radius for --near (default: 25)
```

`--location` does a case-insensitive substring match on `geo_country` and `geo_region`:
```sql
WHERE (geo_country LIKE '%belize%' OR geo_region LIKE '%belize%')
```

`--near` with `--radius` uses SpatiaLite spatial query:
```sql
WHERE ST_Distance(geo_point, MakePoint(?, ?, 4326), 1) <= ?
```
(The `1` flag requests distance in meters on the ellipsoid.)

### Geo data management

```
phig geo init            Download Natural Earth shapefiles and load into DB
phig geo status          Show whether geo data is loaded, how many images have coordinates
```

### Porcelain output

Add `geo_country` and `geo_region` to the porcelain tab-separated format (appended to preserve backward compat, empty string if NULL).

### Format strings (cp/mv)

Add `%country` and `%region` tokens for organize commands:
```
phig cp /archive --format "%Y/%country/%original"
```

## Migration

Existing databases need:
1. `ALTER TABLE images ADD COLUMN` for the new geo columns
2. Reference table creation + shapefile loading
3. Backfill: re-parse GPS EXIF from existing `exif` JSON blobs to populate geo columns without requiring a full `--force` re-scan

The backfill can be a one-time operation during `phig geo init` or triggered automatically on first geo query:
```
phig geo init --backfill    # Load shapefiles + populate geo columns from existing EXIF
```

## Checklist

- [ ] Add `libspatialite` to devenv.nix and CMakeLists.txt
- [ ] GPS EXIF DMS string → decimal degree parser with tests
- [ ] Schema migration: add geo columns to images table
- [ ] SpatiaLite extension loading at DB init
- [ ] Natural Earth shapefile loading into reference tables
- [ ] `phig geo init` command
- [ ] `phig geo status` command
- [ ] Scan-time: parse GPS → store coordinates + reverse geocode
- [ ] Backfill existing images from EXIF JSON
- [ ] `--location` search flag
- [ ] `--near` / `--radius` search flags
- [ ] Porcelain output includes geo fields
- [ ] `%country` / `%region` format string tokens
- [ ] Tests: GPS parsing, reverse geocoding, spatial search, migration

## Open Questions

- **Ship vs. download geodata?** Shipping shapefiles with the binary is simpler but adds ~20MB. Downloading on `geo init` keeps the binary lean but needs a one-time network fetch. Leaning toward download — consistent with "works offline" since you only need the network once.
- **Admin-1 granularity** — Is state/province enough, or do we want city-level resolution too? Populated places can give nearest-city but polygon containment for cities requires a much larger dataset. Could do nearest populated place as a separate `geo_city` column.
- **SpatiaLite static vs. dynamic loading** — Link statically at compile time, or load as a runtime extension? Static linking is simpler for distribution but couples the build. Runtime loading is more flexible but requires the .so to be findable.
