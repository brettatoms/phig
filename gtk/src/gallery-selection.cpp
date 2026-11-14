#include "gallery-selection.h"

#include <memory>
#include <new>
#include <utility>

#include "connect.h"
#include "gref.h"

namespace {

struct BitsetUnref {
    void operator()(GtkBitset* set) const noexcept { gtk_bitset_unref(set); }
};
using Bitset = std::unique_ptr<GtkBitset, BitsetUnref>;

struct State {
    GRef<PhigGalleryModel> model;
    gulong items_changed = 0;
    // Replaced on every change and never changed in place, so a selection
    // already handed out stays as it was.
    Bitset selected{gtk_bitset_new_empty()};
};

}  // namespace

struct _PhigGallerySelection {
    GObject parent_instance;
    State state;  // constructed in phig_gallery_selection_init, destroyed in finalize
};

static void list_model_iface_init(GListModelInterface* iface) noexcept;
static void selection_model_iface_init(GtkSelectionModelInterface* iface) noexcept;

G_DEFINE_FINAL_TYPE_WITH_CODE(PhigGallerySelection, phig_gallery_selection, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, list_model_iface_init)
                                  G_IMPLEMENT_INTERFACE(GTK_TYPE_SELECTION_MODEL,
                                                        selection_model_iface_init))

enum { PROP_0, PROP_MODEL, N_PROPS };
static GParamSpec* props[N_PROPS];

namespace {

State& state_of(gpointer self) { return PHIG_GALLERY_SELECTION(self)->state; }

guint n_items_of(State& s) {
    return s.model ? g_list_model_get_n_items(G_LIST_MODEL(s.model.get())) : 0;
}

Bitset copy_selected(State& s) { return Bitset(gtk_bitset_copy(s.selected.get())); }

// Makes next the selection, limited to the model's positions, and emits
// selection-changed over the span of positions whose state changed, if any.
gboolean replace(gpointer self, Bitset next) {
    State& s = state_of(self);
    gtk_bitset_remove_range_closed(next.get(), n_items_of(s), G_MAXUINT);
    Bitset changed = copy_selected(s);
    gtk_bitset_difference(changed.get(), next.get());
    s.selected = std::move(next);
    if (gtk_bitset_is_empty(changed.get())) return TRUE;
    const guint first = gtk_bitset_get_minimum(changed.get());
    const guint last = gtk_bitset_get_maximum(changed.get());
    gtk_selection_model_selection_changed(GTK_SELECTION_MODEL(self), first, last - first + 1);
    return TRUE;
}

void on_items_changed(PhigGallerySelection* self, guint position, guint removed,
                      guint added) noexcept {
    State& s = self->state;
    // A same-size change replaces rows in place; any other would move positions.
    const bool keep = removed == added;
    Bitset dropped;
    if (!keep) dropped = std::exchange(s.selected, Bitset(gtk_bitset_new_empty()));
    g_list_model_items_changed(G_LIST_MODEL(self), position, removed, added);
    const guint n = n_items_of(s);
    if (!keep && !gtk_bitset_is_empty(dropped.get()) && n > 0)
        gtk_selection_model_selection_changed(GTK_SELECTION_MODEL(self), 0, n);
}

GType get_item_type(GListModel*) noexcept { return PHIG_TYPE_IMAGE_ROW; }

guint get_n_items(GListModel* list) noexcept { return n_items_of(state_of(list)); }

gpointer get_item(GListModel* list, guint position) noexcept {
    State& s = state_of(list);
    return s.model ? g_list_model_get_item(G_LIST_MODEL(s.model.get()), position) : nullptr;
}

gboolean is_selected(GtkSelectionModel* model, guint position) noexcept {
    return gtk_bitset_contains(state_of(model).selected.get(), position);
}

GtkBitset* get_selection_in_range(GtkSelectionModel* model, guint, guint) noexcept {
    return gtk_bitset_ref(state_of(model).selected.get());
}

gboolean select_item(GtkSelectionModel* model, guint position, gboolean unselect_rest) noexcept {
    Bitset next(unselect_rest ? gtk_bitset_new_empty() : copy_selected(state_of(model)).release());
    gtk_bitset_add(next.get(), position);
    return replace(model, std::move(next));
}

gboolean unselect_item(GtkSelectionModel* model, guint position) noexcept {
    Bitset next = copy_selected(state_of(model));
    gtk_bitset_remove(next.get(), position);
    return replace(model, std::move(next));
}

gboolean select_range(GtkSelectionModel* model, guint position, guint n_items,
                      gboolean unselect_rest) noexcept {
    Bitset next(unselect_rest ? gtk_bitset_new_empty() : copy_selected(state_of(model)).release());
    gtk_bitset_add_range(next.get(), position, n_items);
    return replace(model, std::move(next));
}

gboolean unselect_range(GtkSelectionModel* model, guint position, guint n_items) noexcept {
    Bitset next = copy_selected(state_of(model));
    gtk_bitset_remove_range(next.get(), position, n_items);
    return replace(model, std::move(next));
}

gboolean select_all(GtkSelectionModel* model) noexcept {
    return replace(model, Bitset(gtk_bitset_new_range(0, n_items_of(state_of(model)))));
}

gboolean unselect_all(GtkSelectionModel* model) noexcept {
    return replace(model, Bitset(gtk_bitset_new_empty()));
}

gboolean set_selection(GtkSelectionModel* model, GtkBitset* selected, GtkBitset* mask) noexcept {
    Bitset next = copy_selected(state_of(model));
    gtk_bitset_subtract(next.get(), mask);
    Bitset masked(gtk_bitset_copy(selected));
    gtk_bitset_intersect(masked.get(), mask);
    gtk_bitset_union(next.get(), masked.get());
    return replace(model, std::move(next));
}

}  // namespace

static void list_model_iface_init(GListModelInterface* iface) noexcept {
    iface->get_item_type = get_item_type;
    iface->get_n_items = get_n_items;
    iface->get_item = get_item;
}

static void selection_model_iface_init(GtkSelectionModelInterface* iface) noexcept {
    iface->is_selected = is_selected;
    iface->get_selection_in_range = get_selection_in_range;
    iface->select_item = select_item;
    iface->unselect_item = unselect_item;
    iface->select_range = select_range;
    iface->unselect_range = unselect_range;
    iface->select_all = select_all;
    iface->unselect_all = unselect_all;
    iface->set_selection = set_selection;
}

static void phig_gallery_selection_get_property(GObject* obj, guint prop_id, GValue* value,
                                                GParamSpec* pspec) noexcept {
    if (prop_id == PROP_MODEL)
        g_value_set_object(value, state_of(obj).model.get());
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
}

static void phig_gallery_selection_dispose(GObject* obj) noexcept {
    State& s = state_of(obj);
    if (s.items_changed != 0) {
        g_signal_handler_disconnect(s.model.get(), s.items_changed);
        s.items_changed = 0;
    }
    s.model.reset();
    G_OBJECT_CLASS(phig_gallery_selection_parent_class)->dispose(obj);
}

static void phig_gallery_selection_finalize(GObject* obj) noexcept {
    state_of(obj).~State();
    G_OBJECT_CLASS(phig_gallery_selection_parent_class)->finalize(obj);
}

static void phig_gallery_selection_class_init(PhigGallerySelectionClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->get_property = phig_gallery_selection_get_property;
    object_class->dispose = phig_gallery_selection_dispose;
    object_class->finalize = phig_gallery_selection_finalize;

    props[PROP_MODEL] =
        g_param_spec_object("model", nullptr, nullptr, G_TYPE_LIST_MODEL,
                            static_cast<GParamFlags>(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_properties(object_class, N_PROPS, props);
}

static void phig_gallery_selection_init(PhigGallerySelection* self) { new (&self->state) State(); }

PhigGallerySelection* phig_gallery_selection_new(PhigGalleryModel* model) {
    auto* self = PHIG_GALLERY_SELECTION(g_object_new(PHIG_TYPE_GALLERY_SELECTION, nullptr));
    self->state.model = gref(model);
    self->state.items_changed = phig::gtk::connect<void(GListModel*, guint, guint, guint)>(
        model, "items-changed",
        [self](GListModel*, guint position, guint removed, guint added) noexcept {
            on_items_changed(self, position, removed, added);
        });
    return self;
}
