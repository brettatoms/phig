#pragma once

#include <gio/gio.h>

#include <functional>
#include <optional>
#include <utility>

#include "gref.h"

namespace phig::gtk {

// Runs work on GLib's worker pool, then calls done with its result on the
// calling thread's default main context. The default GApplication, if there is
// one, is held until done has run, so a job started from the window keeps the
// process up until it delivers. done must check that the widgets it touches
// still exist.
//
// work runs on a worker thread: it may use only what it captured by value or
// was guaranteed to outlive it, never PhigApp and never a widget. The
// collection qualifies, because the held application keeps main from closing
// it until done has run.
template <typename T>
void run_in_background(std::move_only_function<T() noexcept> work,
                       std::move_only_function<void(T) noexcept> done) {
    struct Job {
        std::move_only_function<T() noexcept> work;
        std::move_only_function<void(T) noexcept> done;
        std::optional<T> result;  // set on the worker thread
        GRef<GApplication> app;   // held from submission until done has run
    };
    auto* job =
        new Job{std::move(work), std::move(done), std::nullopt, gref(g_application_get_default())};
    if (job->app) g_application_hold(job->app.get());

    GTask* task = g_task_new(
        nullptr, nullptr,
        [](GObject*, GAsyncResult* result, gpointer) noexcept {
            auto& j = *static_cast<Job*>(g_task_get_task_data(G_TASK(result)));
            j.done(std::move(*j.result));
            if (j.app) g_application_release(j.app.get());
        },
        nullptr);
    g_task_set_task_data(task, job, [](gpointer p) noexcept { delete static_cast<Job*>(p); });
    g_task_run_in_thread(task, [](GTask* t, gpointer, gpointer data, GCancellable*) noexcept {
        auto& j = *static_cast<Job*>(data);
        j.result.emplace(j.work());
        g_task_return_boolean(t, TRUE);
    });
    g_object_unref(task);
}

}  // namespace phig::gtk
