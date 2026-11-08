#pragma once

#include <string>
#include <vector>

#include "paths.h"

// What migrate_legacy_catalog_files did. Paths are the legacy locations, so a
// caller can log them once its logger is up.
struct LegacyMigration {
    std::vector<std::string> moved;      // now inside the catalog
    std::vector<std::string> kept_both;  // left alone: the catalog already has one
    std::vector<std::string> failed;     // the move failed; left in place
};

// Moves the thumbnails and log that the default catalog keeps inside its
// directory but phig once kept elsewhere: $XDG_CACHE_HOME/phig/thumbs to
// <catalog>/thumbs, and $XDG_STATE_HOME/phig/phig.log and phig.1.log to
// <catalog>/. Only for the default catalog, and only once its database
// exists, so a command against a missing catalog creates nothing.
// Idempotent: once the legacy paths are gone there is nothing to move.
// Never overwrites: a destination that already exists is left, and so is
// the legacy path.
LegacyMigration migrate_legacy_catalog_files(const CatalogPaths& catalog);
