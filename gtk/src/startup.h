#pragma once

#include <expected>
#include <functional>
#include <string>

#include "collection.h"

namespace phig::gtk {

using OpenResult = std::expected<CollectionPtr, OpenError>;
using OpenDone = std::move_only_function<void(OpenResult) noexcept>;

// Opens catalog_dir on a worker thread, then calls done with the result on the
// calling thread's default main context. The worker reads only its own copy of
// catalog_dir, so the caller's state may be freed while it runs. Keeping alive
// whatever done refers to is the caller's job. done may be destroyed on either
// thread.
void open_in_background(std::string catalog_dir, OpenDone done);

}  // namespace phig::gtk
