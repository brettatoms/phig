#include <glib.h>

#include <string>

#include "format.h"

namespace {

using phig::gtk::Details;

// g_assert_cmpstr keeps only the pointer it is given, so a temporary string
// must outlive the macro: here it lives until this call returns.
void assert_text(const std::string& got, const char* want) {
    g_assert_cmpstr(got.c_str(), ==, want);
}

void test_camera_puts_the_make_first_only_when_the_model_lacks_it() {
    Details d;
    assert_text(phig::gtk::camera_text(d), "");
    d.make = "Canon";
    assert_text(phig::gtk::camera_text(d), "Canon");
    d.model = "Canon EOS R5";
    assert_text(phig::gtk::camera_text(d), "Canon EOS R5");
    d.make = "SONY";
    d.model = "ILCE-7M3";
    assert_text(phig::gtk::camera_text(d), "SONY ILCE-7M3");
}

void test_exposure_joins_what_is_known() {
    Details d;
    assert_text(phig::gtk::exposure_text(d), "");
    d.exposure_time = "1/500 sec.";
    d.f_number = "f/4.0";
    d.iso = "400";
    d.focal_length = "24.0 mm";
    assert_text(phig::gtk::exposure_text(d), "1/500 sec. · f/4.0 · ISO 400 · 24.0 mm");
    d.f_number.clear();
    d.focal_length.clear();
    assert_text(phig::gtk::exposure_text(d), "1/500 sec. · ISO 400");
}

// g_format_size puts a no-break space before the unit.
void test_size_gives_dimensions_and_bytes() {
    Details d;
    d.width = 6000;
    d.height = 4000;
    d.file_size = 12300000;
    assert_text(phig::gtk::size_text(d), "6000 × 4000 · 12.3\u00a0MB");
    d.width = 0;
    assert_text(phig::gtk::size_text(d), "12.3\u00a0MB");
}

void test_sharpness_says_when_it_was_not_measured() {
    Details d;
    assert_text(phig::gtk::sharpness_text(d), "Not measured");
    d.blur_score = 6.93;
    assert_text(phig::gtk::sharpness_text(d), "6.9");
}

void test_progress_counts_files_and_failures() {
    phig::gtk::IndexProgress p;
    assert_text(phig::gtk::progress_text(p), "Finding files…");
    p.total = 1892;
    p.done = 342;
    assert_text(phig::gtk::progress_text(p), "342 of 1892 files");
    p.errors = 2;
    assert_text(phig::gtk::progress_text(p), "342 of 1892 files · 2 failed");
}

void test_summary_says_whether_the_scan_finished() {
    phig::gtk::IndexSummary s{15, 12, 3, 0, true};
    assert_text(phig::gtk::summary_text(s), "Scan finished: 12 added or changed, 3 unchanged");
    s.completed = false;
    s.errors = 1;
    assert_text(phig::gtk::summary_text(s),
                "Scan cancelled: 12 added or changed, 3 unchanged, 1 failed");
}

void test_a_refused_write_says_nothing_changed() {
    phig::gtk::WriteError busy{true, "Another phig operation is running (pid 42)"};
    assert_text(phig::gtk::write_error_text(busy),
                "Nothing was changed: Another phig operation is running (pid 42). Try "
                "again when it finishes.");
    phig::gtk::WriteError failed{false, "A tag needs a name."};
    assert_text(phig::gtk::write_error_text(failed), "Nothing was changed: A tag needs a name.");
}

void test_selection_counts_photos() {
    assert_text(phig::gtk::selection_text(1), "1 photo selected");
    assert_text(phig::gtk::selection_text(12), "12 photos selected");
}

}  // namespace

int main(int argc, char** argv) {
    g_test_init(&argc, &argv, nullptr);
    g_test_add_func("/format/camera-puts-the-make-first-only-when-the-model-lacks-it",
                    test_camera_puts_the_make_first_only_when_the_model_lacks_it);
    g_test_add_func("/format/exposure-joins-what-is-known", test_exposure_joins_what_is_known);
    g_test_add_func("/format/size-gives-dimensions-and-bytes",
                    test_size_gives_dimensions_and_bytes);
    g_test_add_func("/format/sharpness-says-when-it-was-not-measured",
                    test_sharpness_says_when_it_was_not_measured);
    g_test_add_func("/format/progress-counts-files-and-failures",
                    test_progress_counts_files_and_failures);
    g_test_add_func("/format/summary-says-whether-the-scan-finished",
                    test_summary_says_whether_the_scan_finished);
    g_test_add_func("/format/a-refused-write-says-nothing-changed",
                    test_a_refused_write_says_nothing_changed);
    g_test_add_func("/format/selection-counts-photos", test_selection_counts_photos);
    return g_test_run();
}
