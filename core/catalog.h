#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "config.h"
#include "database.h"
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

// How a command opens its catalog's database.
enum class CatalogAccess {
    Read,    // read-only; the catalog must exist at this build's schema (R70)
    Write,   // the catalog must exist; migrated to this build's schema
    Create,  // created if missing, then migrated; only scan and libphig's
             // read-write context create a catalog
};

// Why a catalog could not be opened.
class CatalogError : public std::runtime_error {
   public:
    enum class Kind {
        Missing,   // no database in the catalog directory
        Outdated,  // older schema; a read cannot migrate it
        Newer,     // schema newer than this build reads
    };
    CatalogError(Kind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}
    Kind kind() const { return kind_; }

   private:
    Kind kind_;
};

// Opens the catalog database at db_path. Read never creates or migrates
// anything: a missing database, or one at another schema version, throws
// CatalogError. Write throws on a missing database and migrates an older
// one. Create makes a missing database and migrates. All three refuse a
// database newer than this build.
std::unique_ptr<Database> open_catalog_db(const std::string& db_path, CatalogAccess access);

// Throws CatalogError(Missing) unless the database at db_path exists. For
// commands that touch a catalog without opening its database, and before
// taking the writer lock, whose file would otherwise create the directory.
void require_catalog(const std::string& db_path);

// Where a command's catalog came from.
enum class CatalogSource { Flag, Environment, Config, Default };

struct ResolvedCatalog {
    std::string dir;  // absolute, lexically normal
    CatalogSource source;
};

// Which catalog a command uses: the --catalog flag, then PHIG_CATALOG, then
// the config file's [paths] catalog, then default_catalog_dir(). An empty
// PHIG_CATALOG counts as unset; an empty flag or config value throws
// std::runtime_error naming it. A leading ~ is expanded, and a relative path
// is resolved against the working directory.
ResolvedCatalog resolve_catalog_dir(const std::optional<std::string>& flag, const Config& cfg);

// "--catalog", "PHIG_CATALOG", "config" or "default", for messages.
const char* catalog_source_name(CatalogSource source);
