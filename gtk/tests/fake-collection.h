#pragma once

#include <cstdint>
#include <string>

#include "collection.h"

// A Collection with synthetic rows and no database, for the gtk tests.
namespace phig::gtk::fake {

// n_rows rows. Row i (0-based) has id i + 1, path "/fake/<i>.jpg", filename
// "<i>.jpg" and the sha256 sha(i) gives. Thumbnails live in a private temp
// directory that closing the collection removes. Generating one writes a 64x64
// PNG; generation reports NoSuchRow for an id outside [1, n_rows].
CollectionPtr make(uint32_t n_rows);

// Row index's sha256: the index in hex, zero-padded to 64 digits.
std::string sha(uint32_t index);

// The reason page gives while fail_pages is set.
inline constexpr const char* kPageError = "fake: page fetch failed";

// While fail is set, page fails with kPageError.
void fail_pages(Collection& coll, bool fail);

// How many thumbnail files thumb_generate has written.
uint32_t generate_count(Collection& coll);

// While hold is set, thumb_generate waits before generating.
void hold_generation(Collection& coll, bool hold);

// While hold is set, page waits before returning.
void hold_pages(Collection& coll, bool hold);

// How many page calls are inside page right now.
uint32_t pages_in_progress(Collection& coll);

}  // namespace phig::gtk::fake
