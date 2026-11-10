#include <glib-object.h>

#include <utility>

#include "connect.h"

// A GObject with one signal, "ask", that takes an int and returns one, so the
// tests can check arguments, the return value and handler lifetime without GTK.
#define TEST_TYPE_EMITTER (test_emitter_get_type())
G_DECLARE_FINAL_TYPE(TestEmitter, test_emitter, TEST, EMITTER, GObject)

struct _TestEmitter {
    GObject parent_instance;
};

G_DEFINE_FINAL_TYPE(TestEmitter, test_emitter, G_TYPE_OBJECT)

static guint ask_signal;

static void test_emitter_class_init(TestEmitterClass* klass) {
    ask_signal = g_signal_new("ask", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr,
                              nullptr, nullptr, G_TYPE_INT, 1, G_TYPE_INT);
}

static void test_emitter_init(TestEmitter* self) { (void)self; }

namespace {

TestEmitter* new_emitter() { return TEST_EMITTER(g_object_new(TEST_TYPE_EMITTER, nullptr)); }

int ask(TestEmitter* emitter, int question) {
    int answer = 0;
    g_signal_emit(emitter, ask_signal, 0, question, &answer);
    return answer;
}

// A move-only capture that counts its own destruction.
class Probe {
   public:
    explicit Probe(int& destroyed) : destroyed_(&destroyed) {}
    Probe(Probe&& other) noexcept : destroyed_(std::exchange(other.destroyed_, nullptr)) {}
    Probe& operator=(Probe&&) = delete;
    ~Probe() {
        if (destroyed_ != nullptr) ++*destroyed_;
    }

   private:
    int* destroyed_;
};

void test_a_connected_lambda_gets_the_arguments_and_returns_its_value() {
    TestEmitter* emitter = new_emitter();
    TestEmitter* seen = nullptr;
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&seen](TestEmitter* self, int question) noexcept {
                                                   seen = self;
                                                   return question * 2;
                                               });
    g_assert_cmpint(ask(emitter, 21), ==, 42);
    g_assert_true(seen == emitter);
    g_object_unref(emitter);
}

void test_disconnecting_destroys_the_captures() {
    TestEmitter* emitter = new_emitter();
    int destroyed = 0;
    gulong id = phig::gtk::connect<int(TestEmitter*, int)>(
        emitter, "ask", [probe = Probe(destroyed)](TestEmitter*, int) noexcept { return 0; });
    g_assert_cmpint(destroyed, ==, 0);

    g_signal_handler_disconnect(emitter, id);
    g_assert_cmpint(destroyed, ==, 1);

    g_object_unref(emitter);
    g_assert_cmpint(destroyed, ==, 1);
}

void test_finalizing_the_instance_destroys_the_captures() {
    TestEmitter* emitter = new_emitter();
    int destroyed = 0;
    phig::gtk::connect<int(TestEmitter*, int)>(
        emitter, "ask", [probe = Probe(destroyed)](TestEmitter*, int) noexcept { return 0; });

    g_object_unref(emitter);
    g_assert_cmpint(destroyed, ==, 1);
}

void test_each_connection_owns_its_own_captures() {
    TestEmitter* emitter = new_emitter();
    int calls = 0;
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&calls](TestEmitter*, int) noexcept {
                                                   calls++;
                                                   return 1;
                                               });
    phig::gtk::connect<int(TestEmitter*, int)>(emitter, "ask",
                                               [&calls](TestEmitter*, int) noexcept {
                                                   calls++;
                                                   return 2;
                                               });
    g_assert_cmpint(ask(emitter, 0), ==, 2);  // RUN_LAST: the last handler's value wins
    g_assert_cmpint(calls, ==, 2);
    g_object_unref(emitter);
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/connect/a-connected-lambda-gets-the-arguments-and-returns-its-value",
                    test_a_connected_lambda_gets_the_arguments_and_returns_its_value);
    g_test_add_func("/connect/disconnecting-destroys-the-captures",
                    test_disconnecting_destroys_the_captures);
    g_test_add_func("/connect/finalizing-the-instance-destroys-the-captures",
                    test_finalizing_the_instance_destroys_the_captures);
    g_test_add_func("/connect/each-connection-owns-its-own-captures",
                    test_each_connection_owns_its_own_captures);
    return g_test_run();
}
