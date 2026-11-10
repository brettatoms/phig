#include "startup.h"

#include <gio/gio.h>

#include <optional>
#include <utility>

namespace phig::gtk {

namespace {

struct OpenTask {
    std::string catalog_dir;
    std::optional<OpenResult> result;  // set on the worker thread
    OpenDone done;
};

void open_thread(GTask* task, gpointer source_object, gpointer task_data,
                 GCancellable* cancellable) noexcept {
    (void)source_object;
    (void)cancellable;
    auto& data = *static_cast<OpenTask*>(task_data);
    data.result.emplace(open(data.catalog_dir));
    g_task_return_boolean(task, TRUE);
}

void open_finished(GObject* source, GAsyncResult* result, gpointer user_data) noexcept {
    (void)source;
    (void)user_data;
    auto& data = *static_cast<OpenTask*>(g_task_get_task_data(G_TASK(result)));
    data.done(std::move(*data.result));
}

}  // namespace

void open_in_background(std::string catalog_dir, OpenDone done) {
    auto* data = new OpenTask{std::move(catalog_dir), std::nullopt, std::move(done)};
    GTask* task = g_task_new(nullptr, nullptr, open_finished, nullptr);
    g_task_set_task_data(task, data, [](gpointer p) noexcept { delete static_cast<OpenTask*>(p); });
    g_task_run_in_thread(task, open_thread);
    g_object_unref(task);
}

}  // namespace phig::gtk
