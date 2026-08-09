#pragma once

#include <string>
#include <vector>

// Exposed for tests. The containment check is a pure function over a buffer so
// it can be exercised without a real Live Photo — those are personal media and
// cannot be committed as fixtures, so the file-backed tests skip when absent
// and this carries the logic that must always be covered.
bool exif_blob_contains_identifier(const std::vector<unsigned char>& blob, const std::string& id);
