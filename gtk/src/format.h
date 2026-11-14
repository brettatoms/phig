#pragma once

#include <cstdint>
#include <string>

#include "collection.h"

// Text the window shows for values the collection returns.
namespace phig::gtk {

// The model, with the make in front when the model does not already start
// with it: "Canon EOS R5", "SONY ILCE-7M3". Empty when neither is known.
std::string camera_text(const Details& d);

// Exposure time, aperture, ISO and focal length, those that are known, joined
// by " · ": "1/500 sec. · f/4.0 · ISO 400 · 24.0 mm".
std::string exposure_text(const Details& d);

// Pixel dimensions, when known, and file size: "6000 × 4000 · 12.3 MB".
std::string size_text(const Details& d);

// The blur score to one decimal place, or "Not measured".
std::string sharpness_text(const Details& d);

// A running scan: "Finding files…" until the walk has counted them, then
// "342 of 1892 files", with "· 2 failed" once any have.
std::string progress_text(const IndexProgress& p);

// A finished scan: "Scan finished: 12 added or changed, 3 unchanged, 1 failed",
// or "Scan cancelled: …" when it did not complete.
std::string summary_text(const IndexSummary& s);

// A refused write. Busy names the other operation and says nothing changed.
std::string write_error_text(const WriteError& e);

// "1 photo selected", "12 photos selected".
std::string selection_text(uint64_t n);

}  // namespace phig::gtk
