#pragma once

#include <glib-object.h>

#include <type_traits>
#include <utility>

namespace phig::gtk {

namespace detail {

template <typename Sig>
struct Trampoline;

template <typename R, typename... Args>
struct Trampoline<R(Args...)> {
    template <typename F>
    static R call(Args... args, gpointer data) noexcept {
        return (*static_cast<F*>(data))(args...);
    }

    template <typename F>
    static void destroy(gpointer data, GClosure* closure) noexcept {
        (void)closure;
        delete static_cast<F*>(data);
    }

    template <typename F>
    static constexpr bool nothrow = std::is_nothrow_invocable_r_v<R, F&, Args...>;
};

}  // namespace detail

// Connects f to instance's signal and returns the handler id. Sig is the
// signal's C signature without the trailing user_data, such as
// void(GtkGridView*, guint) for GtkGridView::activate. GLib cannot check it
// against the signal, so it must match the signal's documentation.
//
// f moves to the heap and is destroyed when the handler is disconnected or the
// instance is finalized, so it may own its captures. An invalid signal name
// returns 0 and f is destroyed before connect returns. f must be noexcept: no
// exception may unwind into GLib, and the static_assert makes that a compile
// error rather than a review item.
//
// Not named signal.h: src/ is on the include path, and a signal.h there would
// shadow the system header GLib includes.
template <typename Sig, typename F>
gulong connect(gpointer instance, const char* signal, F f) {
    using T = detail::Trampoline<Sig>;
    static_assert(T::template nothrow<F>, "a signal handler must be noexcept");
    auto* owned = new F(std::move(f));
    const gulong id = g_signal_connect_data(instance, signal, G_CALLBACK(&T::template call<F>),
                                            owned, &T::template destroy<F>, GConnectFlags{});
    // An invalid signal name returns 0 without calling the destroy notify.
    if (id == 0) delete owned;
    return id;
}

}  // namespace phig::gtk
