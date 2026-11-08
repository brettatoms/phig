#pragma once

#include <glib-object.h>

#include <memory>

// An owning reference to a GObject. Move-only; destruction drops the ref.
struct GObjectUnref {
    void operator()(gpointer object) const noexcept { g_object_unref(object); }
};

template <class T>
using GRef = std::unique_ptr<T, GObjectUnref>;

// Takes a new reference on object, which may be NULL.
template <class T>
GRef<T> gref(T* object) {
    return GRef<T>(object != nullptr ? static_cast<T*>(g_object_ref(object)) : nullptr);
}

// Adopts a reference the caller already owns, such as a constructor's result.
template <class T>
GRef<T> adopt(T* object) {
    return GRef<T>(object);
}
