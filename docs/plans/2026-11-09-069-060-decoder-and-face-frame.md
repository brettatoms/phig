# Decoder Messages and Face Frames Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the process-wide stderr redirect by decoding JPEG through phig's own libjpeg decoder, have every decode report the frame it produced, and use that frame and the recognition model to carry face labels through a rebuild without ever moving a label to the wrong face.

**Architecture:** Part 1 (Tasks 1-7) is plan 069. A new `core/jpeg_decode.{h,cpp}` decodes JPEG with a per-call libjpeg error manager. All three JPEG paths move onto it: plain files, a RAW's sidecar JPEG and its embedded preview. `DecodeResult` moves to a new `core/decode_result.h` and gains a `DecodeFrame`. Video logs go through FFmpeg's and OpenCV's own log controls, and `StderrSuppressor` is deleted. Part 2 (Tasks 8-12) is plan 060's open question 7 decision:
- Schema v10 adds five provenance columns to `faces`.
- `carry_labels` decides each labelled face by model and frame: the same model matches by embedding; a different model matches by box overlap only within an identical frame; otherwise the label is dropped and counted.
- Every face writer records provenance.

**Tech Stack:** C++20, libjpeg-turbo through the libjpeg API (`jpeglib.h`), OpenCV, SQLite, GoogleTest, Python 3 with PIL for fixture generation.

**Spec:** `plans/069-decoder-messages.md` (Part 1) and `plans/060-silent-loss-on-success.md`, section "Open question 7 decision" (Part 2). Read both before starting a task.

## Global Constraints

- Decoded JPEG pixels match `cv::imread(path, cv::IMREAD_COLOR)` for files and `cv::imdecode(buf, cv::IMREAD_COLOR)` for memory, byte for byte, on the machine the test runs on. Stored perceptual hashes must not move.
- The embedded RAW preview stays as `cv::imdecode` decodes it. Turning it upright is out of scope.
- No new `#ifdef` outside the file that owns a platform concern. `core/` stays portable to Linux, macOS and Windows.
- libjpeg-turbo is found with CMake's `FindJPEG` (`find_package(JPEG REQUIRED)`, target `JPEG::JPEG`).
- Schema goes from v9 to v10. Migrations are forward-only and follow the existing loop in `Database::init_schema` (`core/database.cpp:581-640`).
- The model identifier is the sha256 of the recognition model file. Faces written before v10 are taken to come from `face_recognition_sface_2021dec.onnx`, sha256 `0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79`.
- Two frames are equal when `frame_sha256` (non-empty), `frame_width` and `frame_height` all agree. `frame_source` is recorded and not compared.
- Embedding match threshold: L2 distance 1.128 between unit-normalized embeddings (SFace's published `FR_NORM_L2` threshold). Box overlap threshold stays `kLabelCarryMinOverlap` (0.5).
- Corpus work uses public sources only, fetched to a scratch directory outside the repository. Nothing in the corpus is committed.
- Data safety: never write to or open `~/.local/share/phig/`, `~/.cache/phig/`, `~/.local/state/phig/`, `~/.config/phig/` or `/mnt/data/Pictures`, not even read-only with `sqlite3`. Hand-run `phig` only with `HOME` and the four `XDG_*_HOME` variables exported to a `mktemp -d` scratch directory and `PHIG_CATALOG` unset. The test binaries redirect `HOME` themselves.
- Comments describe the present; prose is plain (root `CLAUDE.md`). Changing a guard updates its `rules/` record in the same commit, then `python3 scripts/rules.py --check --emit` and commit the regenerated `rules/by-*.md`.
- clang-format clean: `clang-format --dry-run --Werror <files>`.
- No ABI change: `libphig/include/phig/phig.h` is not edited.

## File Structure

| File | Responsibility |
|---|---|
| `core/decode_result.h` (new) | `DecodeSource`, `DecodeFrame`, `DecodeResult`: what any image decode returns. |
| `core/jpeg_decode.h`, `core/jpeg_decode.cpp` (new) | JPEG decode through libjpeg, per-call messages, EXIF orientation as OpenCV applies it. |
| `core/hasher.{h,cpp}` | `decode_image_with_warning` dispatch, video keyframe; loses `StderrSuppressor`. |
| `core/raw.{h,cpp}` | RAW tiers return `DecodeResult` with their frame. |
| `core/metadata.cpp`, `core/live_photo.cpp` | FFmpeg log goes through one capture callback. |
| `core/log.cpp` | Comment only. |
| `core/faces.{h,cpp}` | `FaceProvenance`, `recognition_model_sha256()`, `carry_labels` by model and frame. |
| `core/models.{h,cpp}` | `kSfaceModelSha256` shared by the download list and pre-v10 rows. |
| `core/database.{h,cpp}` | v10 migration, provenance on insert and on the labelled-face read. |
| `core/faces_engine.cpp`, `core/scan_engine.{h,cpp}` | Face writers record provenance. |
| `tests/data/jpeg/` (new) | Generated JPEG fixtures and `make_fixtures.py`. |
| `tests/test_jpeg_decode.cpp` (new) | Decoder identity, messages, threads, corpus check. |
| `tests/perf_faces.cpp` (new) | `FacesPerf` rebuild ratio test in `phig-perf-tests`. |
| `rules/authorization.toml`, `rules/metadata.toml` | R60 update, new R86. |

---

### Task 1: Corpus and stderr inventory (plan 069 steps 0 and 1)

A measurement task. It commits only text, recorded in plan 069.

**Files:**
- Modify: `plans/069-decoder-messages.md` (a new `## Inventory` section after `## Steps`)

**Interfaces:**
- Produces: the corpus at `$HOME/phig-corpus/` (outside the repo, not committed), with `$HOME/phig-corpus/SOURCES.md` listing every URL fetched. Tasks 3 and 7 reuse it.

- [ ] **Step 1: Assemble the corpus.** Fetch into `$HOME/phig-corpus/<source>/`:
  - `https://github.com/ianare/exif-samples` (clone, `jpg/` tree);
  - `https://github.com/recurser/exif-orientation-examples` (clone);
  - libjpeg-turbo `testimages/` (`https://github.com/libjpeg-turbo/libjpeg-turbo`, sparse checkout of `testimages`);
  - RAW files with embedded previews from `https://raw.pixls.us`, one per make for at least ten makes;
  - JPEGs from Wikimedia Commons across camera makes and editing software. Use original files, not Commons' rescaled thumbnails, which come from one encoder.
  - PNG, TIFF and WebP files from the same sources, and short videos (MP4, MOV, MKV, WebM) from Wikimedia Commons.

  Stop at 5,000 JPEGs, counting sidecar-style JPEGs, and record the total size. Before fetching more than 10 GB, stop and ask the controller. Write `SOURCES.md` with each URL and its file count.

- [ ] **Step 2: Build with the suppressor disabled.** In a scratch worktree, never committed, make `StderrSuppressor`'s constructor and destructor bodies empty (`core/hasher.cpp:172-185`). Build Release:

  ```bash
  cmake -B build-inv -DCMAKE_BUILD_TYPE=Release && cmake --build build-inv -j
  ```

- [ ] **Step 3: Scan the corpus with stderr captured.**

  ```bash
  S=$(mktemp -d); export HOME=$S XDG_CONFIG_HOME=$S/c XDG_DATA_HOME=$S/d XDG_CACHE_HOME=$S/k XDG_STATE_HOME=$S/s; unset PHIG_CATALOG
  ./build-inv/phig --catalog $S/cat scan $HOME_REAL/phig-corpus --recursive --media all --hidden 2> $S/stderr.txt
  ```

  `$HOME_REAL` is the original `$HOME`, saved before the export.

- [ ] **Step 4: Classify the lines.** Group `stderr.txt` lines by the library that wrote them: libjpeg ("Premature end of JPEG file", "Corrupt JPEG data"), libpng, libtiff, libwebp, FFmpeg (`[mov,mp4,...]`, `[h264 @`), GStreamer and OpenCV (`[ WARN:`), and phig's own `[warning]` lines. Count each group.

- [ ] **Step 5: Record and decide.** Add `## Inventory` to plan 069 with the table, the corpus size, and the pointer to `SOURCES.md`'s contents (copy the URL list in). If libjpeg is not the largest group, or video is not second, stop: the controller rules on revising decisions 2 and 3 before Task 2.

- [ ] **Step 6: Commit.**

  ```bash
  git add plans/069-decoder-messages.md
  git commit -m "Record plan 069's stderr inventory over a public corpus"
  ```

---

### Task 2: The JPEG decoder (plan 069 step 2, decision 5)

**Files:**
- Create: `core/decode_result.h`, `core/jpeg_decode.h`, `core/jpeg_decode.cpp`, `tests/data/jpeg/make_fixtures.py`, `tests/data/jpeg/*.jpg`, `tests/test_jpeg_decode.cpp`
- Modify: `core/hasher.h` (remove `DecodeResult`, include `decode_result.h`), `core/CMakeLists.txt` (source and `JPEG::JPEG`), root `CMakeLists.txt` (`find_package(JPEG REQUIRED)`, test source), `.github/workflows/ci.yml` (add `libjpeg-dev` to every apt list at lines 83, 301, 390, 424; add `jpeg-turbo` to the brew line at 89), `README.md` (apt line 34 `libjpeg-dev`, dnf line 45 `libjpeg-turbo-devel`, pacman line 50 `libjpeg-turbo`, brew line 54 `jpeg-turbo`)

**Interfaces:**
- Produces:

  ```cpp
  // core/decode_result.h
  enum class DecodeSource { File, SidecarJpeg, RawPreview, RawFull };
  struct DecodeFrame {
      DecodeSource source = DecodeSource::File;
      int width = 0, height = 0;   // of the decoded, oriented image
      std::string source_sha256;   // SidecarJpeg and RawPreview, when requested; else empty
  };
  struct DecodeResult {
      cv::Mat img;
      std::string warning;
      DecodeFrame frame;
  };
  const char* decode_source_name(DecodeSource s);  // "file", "sidecar_jpeg", "raw_preview", "raw_full"

  // core/jpeg_decode.h
  bool is_jpeg_signature(std::span<const unsigned char> head);  // FF D8 FF
  DecodeResult decode_jpeg_file(const std::string& path);
  DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes);
  ```

- [ ] **Step 1: Generate the fixtures.** Write `tests/data/jpeg/make_fixtures.py`:

  ```python
  #!/usr/bin/env python3
  """Generates the JPEG decoder fixtures. Run from this directory; the outputs
  are committed, so CI never runs this."""
  import io
  from PIL import Image

  W, H = 64, 48

  def base(mode="RGB"):
      img = Image.new("RGB", (W, H))
      px = img.load()
      for y in range(H):
          for x in range(W):
              px[x, y] = ((x * 4) % 256, (y * 5) % 256, ((x + y) * 3) % 256)
      return img.convert(mode)

  def save(img, name, **kw):
      img.save(name, "JPEG", quality=90, **kw)

  save(base(), "baseline.jpg")
  save(base(), "progressive.jpg", progressive=True)
  save(base("L"), "grayscale.jpg")
  save(base("CMYK"), "cmyk_adobe.jpg")          # PIL writes an Adobe APP14 marker for CMYK
  save(base(), "sub420.jpg", subsampling=2)
  save(base(), "sub444.jpg", subsampling=0)
  for o in range(1, 9):
      exif = Image.Exif()
      exif[0x0112] = o
      save(base(), f"orient_{o}.jpg", exif=exif.tobytes())
  full = open("baseline.jpg", "rb").read()
  open("truncated.jpg", "wb").write(full[: len(full) * 2 // 3])
  buf = io.BytesIO()
  base().save(buf, "PNG")
  open("png_named.jpg", "wb").write(buf.getvalue())
  ```

  Run `cd tests/data/jpeg && python3 make_fixtures.py`. Check `file *.jpg`: `png_named.jpg` is PNG, the rest JPEG.

- [ ] **Step 2: Write the failing tests.** `tests/test_jpeg_decode.cpp`:

  ```cpp
  #include <gtest/gtest.h>

  #include <filesystem>
  #include <fstream>
  #include <iterator>
  #include <opencv2/core.hpp>
  #include <opencv2/imgcodecs.hpp>
  #include <string>
  #include <thread>
  #include <vector>

  #include "jpeg_decode.h"

  namespace fs = std::filesystem;

  namespace {
  fs::path fixture(const std::string& name) {
      return fs::path(__FILE__).parent_path() / "data" / "jpeg" / name;
  }
  std::vector<unsigned char> read_all(const fs::path& p) {
      std::ifstream f(p, std::ios::binary);
      return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  }
  void expect_identical(const cv::Mat& a, const cv::Mat& b) {
      ASSERT_EQ(a.type(), b.type());
      ASSERT_EQ(a.size(), b.size());
      EXPECT_EQ(cv::norm(a, b, cv::NORM_INF), 0.0);
  }
  const char* kDecodable[] = {
      "baseline.jpg", "progressive.jpg", "grayscale.jpg", "cmyk_adobe.jpg", "sub420.jpg",
      "sub444.jpg",   "orient_1.jpg",    "orient_2.jpg",  "orient_3.jpg",   "orient_4.jpg",
      "orient_5.jpg", "orient_6.jpg",    "orient_7.jpg",  "orient_8.jpg",
  };
  }  // namespace

  TEST(JpegDecode, FileMatchesImreadOnEveryFixture) {
      for (const char* name : kDecodable) {
          SCOPED_TRACE(name);
          DecodeResult r = decode_jpeg_file(fixture(name).string());
          expect_identical(r.img, cv::imread(fixture(name).string(), cv::IMREAD_COLOR));
          EXPECT_EQ(r.frame.width, r.img.cols);
          EXPECT_EQ(r.frame.height, r.img.rows);
      }
  }

  TEST(JpegDecode, MemoryMatchesImdecodeOnEveryFixture) {
      for (const char* name : kDecodable) {
          SCOPED_TRACE(name);
          std::vector<unsigned char> bytes = read_all(fixture(name));
          DecodeResult r = decode_jpeg_memory(bytes);
          expect_identical(r.img, cv::imdecode(bytes, cv::IMREAD_COLOR));
      }
  }

  TEST(JpegDecode, TruncatedFileGivesImreadsPartialImageAndTheMessage) {
      std::string p = fixture("truncated.jpg").string();
      DecodeResult r = decode_jpeg_file(p);
      expect_identical(r.img, cv::imread(p, cv::IMREAD_COLOR));
      EXPECT_NE(r.warning.find("Premature end of JPEG file"), std::string::npos) << r.warning;
  }

  TEST(JpegDecode, ANonJpegIsNotClaimed) {
      std::vector<unsigned char> head = read_all(fixture("png_named.jpg"));
      EXPECT_FALSE(is_jpeg_signature(head));
      EXPECT_TRUE(is_jpeg_signature(read_all(fixture("baseline.jpg"))));
  }

  TEST(JpegDecode, GarbageFailsWithAnEmptyImageAndAMessage) {
      std::vector<unsigned char> junk = {0xFF, 0xD8, 0xFF, 0x00, 0x01, 0x02};
      DecodeResult r = decode_jpeg_memory(junk);
      EXPECT_TRUE(r.img.empty());
      EXPECT_FALSE(r.warning.empty());
  }

  TEST(JpegDecode, ConcurrentDecodesKeepTheirOwnMessages) {
      std::string bad = fixture("truncated.jpg").string();
      std::string good = fixture("baseline.jpg").string();
      std::vector<std::string> bad_warnings(200), good_warnings(200);
      std::thread a([&] {
          for (auto& w : bad_warnings) w = decode_jpeg_file(bad).warning;
      });
      std::thread b([&] {
          for (auto& w : good_warnings) w = decode_jpeg_file(good).warning;
      });
      a.join();
      b.join();
      for (const auto& w : bad_warnings) EXPECT_FALSE(w.empty());
      for (const auto& w : good_warnings) EXPECT_TRUE(w.empty()) << w;
  }
  ```

  Add `tests/test_jpeg_decode.cpp` to the `phig-tests` source list in the root `CMakeLists.txt`.

- [ ] **Step 3: Run to verify failure.** `cmake --build build -j` fails: `jpeg_decode.h` not found.

- [ ] **Step 4: Implement.** Create `core/decode_result.h` with the types above, plus `decode_source_name`, defined inline. Remove `DecodeResult` from `core/hasher.h` and include `decode_result.h` there. Create `core/jpeg_decode.h` with the declarations above, each with a two-line comment. Create `core/jpeg_decode.cpp`:

  ```cpp
  #include "jpeg_decode.h"

  #include <csetjmp>
  #include <cstdint>
  #include <cstdio>
  #include <cstring>
  #include <fstream>
  #include <iterator>
  #include <opencv2/core.hpp>
  #include <vector>

  extern "C" {
  #include <jpeglib.h>
  }

  namespace {

  struct ErrorManager {
      jpeg_error_mgr pub;
      std::jmp_buf jump;
      std::string* messages;
  };

  void append_message(j_common_ptr cinfo) {
      auto* err = reinterpret_cast<ErrorManager*>(cinfo->err);
      char buf[JMSG_LENGTH_MAX];
      (*cinfo->err->format_message)(cinfo, buf);
      if (!err->messages->empty()) *err->messages += "; ";
      *err->messages += buf;
  }

  // libjpeg calls emit_message for warnings (level -1) and trace output
  // (level >= 0); only warnings are kept, as OpenCV's handler shows only those.
  void emit_message(j_common_ptr cinfo, int level) {
      if (level < 0) append_message(cinfo);
  }

  [[noreturn]] void error_exit(j_common_ptr cinfo) {
      append_message(cinfo);
      std::longjmp(reinterpret_cast<ErrorManager*>(cinfo->err)->jump, 1);
  }

  // EXIF orientation (tag 0x0112) from an APP1 "Exif\0\0" marker, 1 when absent.
  int exif_orientation(jpeg_decompress_struct& cinfo) {
      for (jpeg_saved_marker_ptr m = cinfo.marker_list; m != nullptr; m = m->next) {
          if (m->marker != JPEG_APP0 + 1 || m->data_length < 14) continue;
          const unsigned char* d = m->data;
          if (std::memcmp(d, "Exif\0\0", 6) != 0) continue;
          const unsigned char* t = d + 6;
          size_t n = m->data_length - 6;
          bool le = t[0] == 'I' && t[1] == 'I';
          if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
          auto u16 = [&](size_t o) -> unsigned {
              return le ? t[o] | (t[o + 1] << 8) : (t[o] << 8) | t[o + 1];
          };
          auto u32 = [&](size_t o) -> uint32_t {
              return le ? t[o] | (t[o + 1] << 8) | (t[o + 2] << 16) | (uint32_t{t[o + 3]} << 24)
                        : (uint32_t{t[o]} << 24) | (t[o + 1] << 16) | (t[o + 2] << 8) | t[o + 3];
          };
          uint32_t ifd = u32(4);
          if (ifd + 2 > n) return 1;
          unsigned count = u16(ifd);
          for (unsigned i = 0; i < count; ++i) {
              size_t e = ifd + 2 + 12 * size_t{i};
              if (e + 12 > n) return 1;
              if (u16(e) == 0x0112) {
                  unsigned v = u16(e + 8);
                  return (v >= 1 && v <= 8) ? static_cast<int>(v) : 1;
              }
          }
          return 1;
      }
      return 1;
  }

  // The same transforms, in the same order, as OpenCV's ApplyExifOrientation.
  void apply_orientation(cv::Mat& img, int orientation) {
      switch (orientation) {
          case 2: cv::flip(img, img, 1); break;
          case 3: cv::flip(img, img, -1); break;
          case 4: cv::flip(img, img, 0); break;
          case 5: cv::transpose(img, img); break;
          case 6: cv::transpose(img, img); cv::flip(img, img, 1); break;
          case 7: cv::transpose(img, img); cv::flip(img, img, -1); break;
          case 8: cv::transpose(img, img); cv::flip(img, img, 0); break;
          default: break;
      }
  }

  // OpenCV's CMYK to BGR conversion for libjpeg's inverted Adobe CMYK output.
  void cmyk_to_bgr(const unsigned char* cmyk, unsigned char* bgr, int width) {
      for (int i = 0; i < width; ++i, cmyk += 4, bgr += 3) {
          int c = cmyk[0], m = cmyk[1], y = cmyk[2], k = cmyk[3];
          c = k - ((255 - c) * k >> 8);
          m = k - ((255 - m) * k >> 8);
          y = k - ((255 - y) * k >> 8);
          bgr[2] = static_cast<unsigned char>(c);
          bgr[1] = static_cast<unsigned char>(m);
          bgr[0] = static_cast<unsigned char>(y);
      }
  }

  DecodeResult decode(std::span<const unsigned char> bytes, bool apply_exif) {
      DecodeResult result;
      jpeg_decompress_struct cinfo{};
      ErrorManager err{};
      err.messages = &result.warning;
      cinfo.err = jpeg_std_error(&err.pub);
      err.pub.error_exit = error_exit;
      err.pub.emit_message = emit_message;
      // Locals touched after setjmp returns again are volatile or re-read.
      cv::Mat img;
      if (setjmp(err.jump) != 0) {
          jpeg_destroy_decompress(&cinfo);
          result.img.release();
          return result;
      }
      jpeg_create_decompress(&cinfo);
      jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
      jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xffff);
      jpeg_read_header(&cinfo, TRUE);
      const bool cmyk = cinfo.num_components == 4;
      cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_BGR;
      jpeg_start_decompress(&cinfo);
      img.create(static_cast<int>(cinfo.output_height), static_cast<int>(cinfo.output_width),
                 CV_8UC3);
      std::vector<unsigned char> row(cmyk ? cinfo.output_width * 4 : 0);
      while (cinfo.output_scanline < cinfo.output_height) {
          unsigned char* dst = img.ptr<unsigned char>(static_cast<int>(cinfo.output_scanline));
          JSAMPROW rp = cmyk ? row.data() : dst;
          jpeg_read_scanlines(&cinfo, &rp, 1);
          if (cmyk) cmyk_to_bgr(row.data(), dst, static_cast<int>(cinfo.output_width));
      }
      int orientation = apply_exif ? exif_orientation(cinfo) : 1;
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      apply_orientation(img, orientation);
      result.img = img;
      result.frame.width = img.cols;
      result.frame.height = img.rows;
      return result;
  }

  }  // namespace

  bool is_jpeg_signature(std::span<const unsigned char> head) {
      return head.size() >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
  }

  DecodeResult decode_jpeg_memory(std::span<const unsigned char> bytes) {
      return decode(bytes, /*apply_exif=*/true);
  }

  DecodeResult decode_jpeg_file(const std::string& path) {
      std::ifstream f(path, std::ios::binary);
      std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(f),
                                       std::istreambuf_iterator<char>()};
      if (!f.good() && !f.eof()) return DecodeResult{{}, "cannot read " + path, {}};
      return decode(bytes, /*apply_exif=*/true);
  }
  ```

  This code is a draft built from OpenCV's `modules/imgcodecs/src/grfmt_jpeg.cpp` and `loadsave.cpp`, and the identity tests decide whether it is right. Before changing anything, read those two files at the tag of the OpenCV installed here (`opencv_version`), and mirror their settings exactly:
  - the output colour space for 1, 3 and 4 components;
  - any `dct_method` or upsampling setting;
  - the CMYK conversion;
  - whether `imdecode` applies EXIF orientation;
  - what a truncated file returns.

  If `imdecode` does not apply orientation at this version, `decode_jpeg_memory` must not either. Pass `apply_exif=false` there and note it in the header comment. Two more places the draft may need to change:
  - If `emit_message`'s filtering doesn't match the messages OpenCV's stderr shows, follow OpenCV.
  - The draft holds `img` and the vector across `setjmp`; if `-Wclobbered` fires, move the decode body into a helper that `setjmp` guards.

  Add to the root `CMakeLists.txt` after `find_package(OpenCV ...)`: `find_package(JPEG REQUIRED)`. Add `jpeg_decode.cpp` to `phig-core`'s sources and `JPEG::JPEG` to its `target_link_libraries`.

- [ ] **Step 5: Run the tests.** `cmake --build build -j && ./build/phig-tests --gtest_filter='JpegDecode.*'`. Expected: all pass. Fix the decoder, never the expectations, until they do.

- [ ] **Step 6: Package lists.** Edit the CI apt lists, the brew line, and `README.md`'s four lines as listed under Files.

- [ ] **Step 7: Full suite and format.** `ctest --test-dir build --parallel`, then `clang-format --dry-run --Werror core/decode_result.h core/jpeg_decode.h core/jpeg_decode.cpp core/hasher.h tests/test_jpeg_decode.cpp`.

- [ ] **Step 8: Commit.**

  ```bash
  git add core/decode_result.h core/jpeg_decode.h core/jpeg_decode.cpp core/hasher.h core/CMakeLists.txt CMakeLists.txt tests/test_jpeg_decode.cpp tests/data/jpeg .github/workflows/ci.yml README.md
  git commit -m "Decode JPEG through libjpeg with per-call messages"
  ```

---

### Task 3: Pixel identity on the corpus (plan 069 step 3)

**Files:**
- Modify: `tests/test_jpeg_decode.cpp` (one test), `plans/069-decoder-messages.md` (record the result)

**Interfaces:**
- Consumes: `decode_jpeg_file`, `decode_jpeg_memory` from Task 2; the corpus from Task 1.

- [ ] **Step 1: Add the corpus test.** It skips unless `PHIG_JPEG_CORPUS` names a directory:

  ```cpp
  TEST(JpegCorpus, EveryJpegMatchesOpenCv) {
      const char* dir = std::getenv("PHIG_JPEG_CORPUS");
      if (dir == nullptr) GTEST_SKIP() << "PHIG_JPEG_CORPUS not set";
      size_t checked = 0, mismatched = 0;
      for (const auto& e : fs::recursive_directory_iterator(dir)) {
          if (!e.is_regular_file()) continue;
          std::vector<unsigned char> bytes = read_all(e.path());
          if (!is_jpeg_signature(bytes)) continue;
          ++checked;
          cv::Mat want = cv::imread(e.path().string(), cv::IMREAD_COLOR);
          cv::Mat got = decode_jpeg_file(e.path().string()).img;
          bool same = want.empty() ? got.empty()
                                   : (want.type() == got.type() && want.size() == got.size() &&
                                      cv::norm(want, got, cv::NORM_INF) == 0.0);
          if (!same) {
              ++mismatched;
              ADD_FAILURE() << e.path();
          }
      }
      std::cout << "checked " << checked << ", mismatched " << mismatched << "\n";
      EXPECT_GT(checked, 0u);
  }
  ```

  Add `#include <cstdlib>` and `#include <iostream>`.

- [ ] **Step 2: Extract the RAW previews.** For each RAW in the corpus, write its embedded preview to `$HOME/phig-corpus/previews/<name>.jpg` using `dcraw_emu -e` or LibRaw's `simple_dcraw -e`. The test then covers the preview path through `decode_jpeg_file`, which shares `decode` with the memory path.

- [ ] **Step 3: Run it.** `PHIG_JPEG_CORPUS=$HOME/phig-corpus ./build/phig-tests --gtest_filter=JpegCorpus.*`. Expected: `checked` at least 5000, `mismatched 0`. On a mismatch, fix the decoder and rerun Task 2's tests too.

- [ ] **Step 4: Record and commit.** Add the counts, the date and the commit to plan 069 under step 3.

  ```bash
  git add tests/test_jpeg_decode.cpp plans/069-decoder-messages.md
  git commit -m "Check the JPEG decoder against OpenCV on a public corpus"
  ```

---

### Task 4: Switch the call sites and report the frame (plan 069 step 4)

**Files:**
- Modify: `core/hasher.h`, `core/hasher.cpp:267-313`, `core/raw.h`, `core/raw.cpp`, `tests/test_raw.cpp`, `tests/test_hasher.cpp`

**Interfaces:**
- Consumes: Task 2's types and decoder.
- Produces:

  ```cpp
  // core/hasher.h
  // hash_source: also fill frame.source_sha256 for a sidecar JPEG or an
  // embedded preview. Costs a sha256 over those bytes; ask only when the
  // frame will be recorded.
  DecodeResult decode_image_with_warning(const std::string& path, bool hash_source = false);
  // core/raw.h
  DecodeResult raw_decode_result(const std::string& raw_path, bool hash_source = false);
  cv::Mat raw_decode(const std::string& raw_path);  // raw_decode_result(raw_path).img
  ```

  After this task, every `DecodeResult` has `frame.width`/`frame.height` equal to `img.cols`/`img.rows`, and `frame.source` set: `File` for a non-RAW file, otherwise the RAW tier that produced the image.

- [ ] **Step 1: Write the failing tests.** In `tests/test_raw.cpp`:

  ```cpp
  TEST_F(RawDecodeTest, ReportsTheSidecarFrameAndHashesItOnlyWhenAsked) {
      std::string jpg = create_image("IMG_2.JPG", 64, 48, cv::Scalar(10, 20, 30));
      fs::path fixture = fs::path(__FILE__).parent_path() / "data" / "truncated_ojpeg.cr2";
      fs::path raw_path = fs::path(test_dir) / "IMG_2.CR2";
      fs::copy_file(fixture, raw_path);

      DecodeResult plain = raw_decode_result(raw_path.string());
      EXPECT_EQ(plain.frame.source, DecodeSource::SidecarJpeg);
      EXPECT_EQ(plain.frame.width, 64);
      EXPECT_EQ(plain.frame.height, 48);
      EXPECT_TRUE(plain.frame.source_sha256.empty());

      DecodeResult hashed = raw_decode_result(raw_path.string(), /*hash_source=*/true);
      EXPECT_EQ(hashed.frame.source_sha256, compute_sha256(jpg));
      EXPECT_EQ(cv::norm(hashed.img, cv::imread(jpg, cv::IMREAD_COLOR), cv::NORM_INF), 0.0);
  }
  ```

  Add `#include "hasher.h"`. In `tests/test_hasher.cpp`:

  ```cpp
  TEST(DecodeFrame, APlainJpegReportsItsOwnFrame) {
      fs::path p = fs::path(__FILE__).parent_path() / "data" / "jpeg" / "orient_6.jpg";
      DecodeResult r = decode_image_with_warning(p.string(), /*hash_source=*/true);
      ASSERT_FALSE(r.img.empty());
      EXPECT_EQ(r.frame.source, DecodeSource::File);
      EXPECT_EQ(r.frame.width, r.img.cols);
      EXPECT_EQ(r.frame.height, r.img.rows);
      EXPECT_EQ(r.frame.width, 48) << "orientation 6 transposes the 64x48 fixture";
      EXPECT_TRUE(r.frame.source_sha256.empty()) << "a file's own bytes are its row's sha256";
  }

  TEST(DecodeFrame, ANonJpegFileStillReportsItsFrame) {
      fs::path p = fs::path(__FILE__).parent_path() / "data" / "jpeg" / "png_named.jpg";
      DecodeResult r = decode_image_with_warning(p.string());
      ASSERT_FALSE(r.img.empty());
      EXPECT_EQ(r.frame.source, DecodeSource::File);
      EXPECT_EQ(r.frame.width, 64);
  }
  ```

- [ ] **Step 2: Run to verify failure.** Build fails: `raw_decode_result` and `DecodeSource` are unknown in those files.

- [ ] **Step 3: Implement.**
  - `decode_image_with_warning`:
    - RAW extension → `raw_decode_result(path, hash_source)`, keeping the existing "Cannot decode RAW file" warning when the image is empty.
    - Codec extension → `codec_decode`, frame `File`.
    - Otherwise, read the first three bytes. A JPEG signature → `decode_jpeg_file`. Anything else → `cv::imread` under the existing suppressor and thread-local handler; the suppressor goes in Task 6.
    - Set `frame.width`/`frame.height` from `img` on every path.
  - `core/raw.cpp`:
    - `try_sidecar_jpeg(raw_path, hash_source)` reads the candidate file into a byte vector. If `hash_source`, it hashes the vector with `sha256_init`/`sha256_update`/`sha256_final`, then decodes it with `decode_jpeg_memory` and sets source `SidecarJpeg`. A sidecar is a file, and Task 2's test showed file and memory decode identically. If the bytes are not a JPEG (no signature), it uses `cv::imread` on the path as before.
    - `try_embedded_preview(ip, hash_source)` decodes `thumb.thumb` with `decode_jpeg_memory`, hashes the same bytes when asked, and sets source `RawPreview`.
    - `try_full_decode` sets source `RawFull`.
    - `raw_decode_result` runs the tiers in the existing order. `raw_decode` returns its `.img`.
  - Move no other call site.

- [ ] **Step 4: Run.** `./build/phig-tests --gtest_filter='RawDecode*:DecodeFrame.*:JpegDecode.*'`, then the full `ctest --test-dir build --parallel`. The existing scan, thumbnail and faces tests must pass unchanged.

- [ ] **Step 5: Format and commit.**

  ```bash
  git add core/hasher.h core/hasher.cpp core/raw.h core/raw.cpp tests/test_raw.cpp tests/test_hasher.cpp
  git commit -m "Decode every JPEG path through phig's decoder and report the frame"
  ```

---

### Task 5: Video logs through their callbacks (plan 069 step 5, decision 3)

The inventory from Task 1 decides the details. The default below is decision 3's expected shape. If the inventory showed something else, the controller's ruling replaces this task's code.

**Files:**
- Modify: `core/hasher.h`, `core/hasher.cpp` (`extract_video_keyframe`), `core/metadata.cpp:62`, `core/live_photo.cpp:49`, `tests/test_video.cpp`
- Create: `core/av_log_capture.h`, `core/av_log_capture.cpp`

**Interfaces:**
- Produces:

  ```cpp
  // core/av_log_capture.h
  // Installs FFmpeg's log callback once. While a ScopedAvLogCapture is alive on
  // a thread, FFmpeg messages from that thread are appended to its buffer;
  // every other message is dropped.
  class ScopedAvLogCapture {
     public:
      explicit ScopedAvLogCapture(std::string& sink);
      ~ScopedAvLogCapture();
  };
  void install_av_log_callback();  // idempotent
  // core/hasher.h, VideoKeyframe gains:
  std::string warning;  // messages from the decoders while extracting the frame
  ```

- [ ] **Step 1: Write the failing test** in `tests/test_video.cpp`. It truncates a fixture video, extracts the keyframe, and asserts that the messages reach `kf.warning` and none reach fd 2. Use the fd-2 pipe helper below; Task 6 reuses it.

  ```cpp
  // Captures what this process writes to fd 2 while `fn` runs.
  std::string capture_fd2(const std::function<void()>& fn) {
      int pipefd[2];
      EXPECT_EQ(pipe(pipefd), 0);
      int saved = dup(2);
      dup2(pipefd[1], 2);
      close(pipefd[1]);
      fn();
      dup2(saved, 2);
      close(saved);
      std::string out;
      char buf[4096];
      ssize_t n;
      fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
      while ((n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
      close(pipefd[0]);
      return out;
  }

  TEST(VideoKeyframe, ATruncatedVideosMessagesReachItsResultNotStderr) {
      fs::path dir = make_temp_dir("video_log");
      fs::path src = fs::path(__FILE__).parent_path() / "data" / "test_red.mov";
      fs::path cut = dir / "cut.mov";
      std::vector<char> bytes(fs::file_size(src));
      std::ifstream(src, std::ios::binary).read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      std::ofstream(cut, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2));
      VideoKeyframe kf;
      std::string err = capture_fd2([&] { kf = extract_video_keyframe(cut.string()); });
      EXPECT_FALSE(kf.warning.empty());
      EXPECT_TRUE(err.empty()) << err;
      fs::remove_all(dir);
  }
  ```

  Put `capture_fd2` in a new `tests/capture_fd2.h` so Task 6 can include it.

- [ ] **Step 2: Run to verify failure.** The build fails because `kf.warning` doesn't exist. Once it builds, the test fails if fd 2 receives output.

- [ ] **Step 3: Implement.**
  - `av_log_capture.cpp`:
    - A `thread_local std::string* t_sink`.
    - A callback that, when `t_sink` is set and `level <= AV_LOG_WARNING`, formats with `av_log_format_line2` and appends; every other message is dropped.
    - `install_av_log_callback` calls `av_log_set_callback` once under `std::call_once`.
  - Replace both `av_log_set_level(AV_LOG_QUIET)` calls with `install_av_log_callback()`.
  - In `extract_video_keyframe`:
    - Call `install_av_log_callback()`.
    - Hold a `ScopedAvLogCapture` for the decode.
    - Set OpenCV's log level once to errors only with `cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR)` under `std::call_once`, including `<opencv2/core/utils/logger.hpp>`.
    - Keep the `StderrSuppressor` line until Task 6. The test then shows whether anything still reaches fd 2 without it: run with the line removed locally to check, then restore it.
  - Surface `kf.warning` where `compute_video_phash`'s callers already route warnings; if none does, leave it on the struct for Task 6.

- [ ] **Step 4: Run** `./build/phig-tests --gtest_filter='VideoKeyframe.*:*Video*'`, then the full suite.

- [ ] **Step 5: Commit.**

  ```bash
  git add core/av_log_capture.h core/av_log_capture.cpp core/hasher.h core/hasher.cpp core/metadata.cpp core/live_photo.cpp core/CMakeLists.txt tests/test_video.cpp tests/capture_fd2.h
  git commit -m "Capture video decoder logs through FFmpeg's and OpenCV's own controls"
  ```

---

### Task 6: Delete the suppressor (plan 069 step 6, decision 4)

**Files:**
- Modify: `core/hasher.cpp` (delete `StderrSuppressor` at 164-200 and both uses), `core/log.cpp:21-24` (comment), `tests/test_hasher.cpp`

- [ ] **Step 1: Write the failing test** in `tests/test_hasher.cpp`. While another thread decodes a truncated JPEG in a loop, a write to fd 2 from the test thread must arrive:

  ```cpp
  TEST(DecodeStderr, AnotherThreadsStderrIsNotHiddenDuringADecode) {
      fs::path bad = fs::path(__FILE__).parent_path() / "data" / "jpeg" / "truncated.jpg";
      std::atomic<bool> stop{false};
      std::thread decoder([&] {
          while (!stop) decode_image_with_warning(bad.string());
      });
      std::string got = capture_fd2([] {
          for (int i = 0; i < 200; ++i) {
              ::write(2, "x", 1);
              std::this_thread::sleep_for(std::chrono::microseconds(50));
          }
      });
      stop = true;
      decoder.join();
      EXPECT_EQ(got.size(), 200u);
  }
  ```

  `truncated.jpg` no longer reaches the suppressor after Task 4. Run the test once against a PNG decode loop as well (a truncated PNG made with `head -c`), because `cv::imread` still runs under the suppressor for non-JPEG formats. Keep the PNG variant as a second test, `AnotherThreadsStderrIsNotHiddenDuringANonJpegDecode`.

- [ ] **Step 2: Run to verify failure.** The PNG variant loses writes while the suppressor redirects fd 2.

- [ ] **Step 3: Implement.**
  - Delete `StderrSuppressor` and its two uses, and the `<fcntl.h>`, `<unistd.h>` and `<mutex>` includes if nothing else uses them.
  - In `core/log.cpp`, keep the fd-2 duplicate only if something else needs it. Otherwise write the stderr sink to `stderr`, and replace the comment with one in the present tense.
  - Update root `CLAUDE.md`'s sanitizer note if it mentions the suppressor (`grep -n Suppressor CLAUDE.md`).

- [ ] **Step 4: Run** the full `ctest --test-dir build --parallel`.

- [ ] **Step 5: Commit.**

  ```bash
  git add core/hasher.cpp core/log.cpp tests/test_hasher.cpp CLAUDE.md
  git commit -m "Delete StderrSuppressor"
  ```

---

### Task 7: Re-measure and document (plan 069 steps 7 and 8)

**Files:**
- Modify: `plans/069-decoder-messages.md`, `plans/000-INDEX.md`, root `CLAUDE.md`, `README.md` if not done in Task 2

- [ ] **Step 1: Measure.** Build Release twice: at the branch's base (`git merge-base HEAD master`) and at `HEAD`. Copy 600 JPEGs from the corpus to tmpfs (`/dev/shm/phig-bench`). For each build and each of `--parallel 1`, `4` and `12`, run three times under the scratch-HOME exports from Task 1:

  ```bash
  /usr/bin/time -f %e ./build-X/phig --catalog $S/cat-X scan /dev/shm/phig-bench --force hash --parallel N
  ```

  Scan once first to create the catalog, so `--force hash` has rows to redo. Record the median of each cell.

- [ ] **Step 2: Record.** Add the table to plan 069 under step 7, with the machine, the corpus subset and both commits.

- [ ] **Step 3: Docs.**
  - Root `CLAUDE.md`, Tech stack: "JPEG decodes through phig's own libjpeg-turbo decoder (`core/jpeg_decode.cpp`); other formats through OpenCV, libheif, libjxl or ImageIO."
  - Add libjpeg-turbo to the dependency list.
  - `core/log.cpp` was done in Task 6.

- [ ] **Step 4: Status.** Plan 069 goes to `completed` with `completed: <date>` in its frontmatter and its index row. Re-read both and confirm they agree.

- [ ] **Step 5: Commit.**

  ```bash
  git add plans/069-decoder-messages.md plans/000-INDEX.md CLAUDE.md README.md
  git commit -m "Record plan 069's measurements and mark it completed"
  ```

---

### Task 8: Schema v10, face provenance columns

**Files:**
- Modify: `core/faces.h`, `core/database.h`, `core/database.cpp` (`CURRENT_SCHEMA_VERSION` line 23, `migrations()`, fresh `CREATE TABLE faces` at 522-533, `insert_face` at 1820, `get_labelled_faces_for_image` at 1780), `tests/test_database.cpp`, root `CLAUDE.md` ("Current: v9" → v10)

**Interfaces:**
- Produces:

  ```cpp
  // core/faces.h
  // Where a face's box and embedding came from. Empty strings and zero sizes
  // mean unknown, as on rows written before schema v10.
  struct FaceProvenance {
      std::string frame_source;   // decode_source_name(), or empty
      int frame_width = 0, frame_height = 0;
      std::string frame_sha256;   // bytes decoded; empty if unknown
      std::string model_sha256;   // recognition model file; empty if unknown
  };
  struct LabelledFace {
      FaceInfo face;
      int64_t person_id;
      FaceProvenance provenance;
  };
  // core/database.h
  int64_t insert_face(int64_t image_id, const FaceInfo& face, const FaceProvenance& provenance);
  int64_t insert_face(int64_t image_id, const FaceInfo& face);  // unknown provenance
  ```

  Columns: `frame_source TEXT`, `frame_width INTEGER`, `frame_height INTEGER`, `frame_sha256 TEXT`, `model_sha256 TEXT`, all nullable. Empty strings and zero sizes are stored as NULL and read back as empty or zero.

- [ ] **Step 1: Write the failing tests** in `tests/test_database.cpp`. Use the existing helpers there for creating an image row; the snippets below call `add_test_image(db, path)` for it.

  ```cpp
  TEST_F(DatabaseTest, FaceProvenanceRoundTrips) {
      Database db(db_path);
      db.init_schema();
      int64_t image_id = add_test_image(db, "/x/a.jpg");
      FaceInfo f{};
      f.width = f.height = 10;
      FaceProvenance p{"sidecar_jpeg", 640, 480, std::string(64, 'a'), std::string(64, 'b')};
      int64_t id = db.insert_face(image_id, f, p);
      ASSERT_TRUE(db.set_face_person(id, db.get_or_create_person("Ada")));
      std::vector<LabelledFace> got = db.get_labelled_faces_for_image(image_id);
      ASSERT_EQ(got.size(), 1u);
      EXPECT_EQ(got[0].provenance.frame_source, "sidecar_jpeg");
      EXPECT_EQ(got[0].provenance.frame_width, 640);
      EXPECT_EQ(got[0].provenance.frame_height, 480);
      EXPECT_EQ(got[0].provenance.frame_sha256, std::string(64, 'a'));
      EXPECT_EQ(got[0].provenance.model_sha256, std::string(64, 'b'));
  }

  TEST_F(DatabaseTest, AFaceWithUnknownProvenanceReadsBackEmpty) {
      Database db(db_path);
      db.init_schema();
      int64_t image_id = add_test_image(db, "/x/b.jpg");
      FaceInfo f{};
      int64_t id = db.insert_face(image_id, f);
      ASSERT_TRUE(db.set_face_person(id, db.get_or_create_person("Ada")));
      FaceProvenance p = db.get_labelled_faces_for_image(image_id).at(0).provenance;
      EXPECT_TRUE(p.frame_source.empty());
      EXPECT_EQ(p.frame_width, 0);
      EXPECT_TRUE(p.frame_sha256.empty());
      EXPECT_TRUE(p.model_sha256.empty());
  }
  ```

  Add a migration test that follows the existing v9-migration test's pattern (`grep -n "v8\|v9\|Migrat" tests/test_database.cpp`):
  - build a v9 database containing a labelled face;
  - open it with this code;
  - assert the version is 10, the face and its `person_id` survive, and its provenance reads back empty.

  Name it `MigrationToV10KeepsLabelledFacesWithUnknownProvenance`.

- [ ] **Step 2: Run to verify failure.** The build fails: no `FaceProvenance`, no 3-argument `insert_face`.

- [ ] **Step 3: Implement.**
  - `CURRENT_SCHEMA_VERSION = 10`.
  - Append to `migrations()`:

    ```cpp
    // v10: where each face's box and embedding came from, for label carry
    R"(ALTER TABLE faces ADD COLUMN frame_source TEXT;
       ALTER TABLE faces ADD COLUMN frame_width INTEGER;
       ALTER TABLE faces ADD COLUMN frame_height INTEGER;
       ALTER TABLE faces ADD COLUMN frame_sha256 TEXT;
       ALTER TABLE faces ADD COLUMN model_sha256 TEXT;)",
    ```

  - Add the five columns to the fresh `CREATE TABLE faces` after `person_id`.
  - `insert_face(image_id, face, provenance)` binds the five values, binding NULL for an empty string or a zero size. The 2-argument overload calls it with `FaceProvenance{}`. Its comment says every face writer in `core/` passes provenance.
  - `get_labelled_faces_for_image` selects the five columns and fills `provenance`, treating NULL as empty or zero.
  - Check the v7 `faces_new` rebuild migration (`core/database.cpp:124-134`). It runs only on databases below v7, before these columns exist, so it needs no change. Confirm that by reading it.

- [ ] **Step 4: Run** `./build/phig-tests --gtest_filter='Database*'`, then the full suite.

- [ ] **Step 5: Commit.**

  ```bash
  git add core/faces.h core/database.h core/database.cpp tests/test_database.cpp CLAUDE.md
  git commit -m "Record where each face came from (schema v10)"
  ```

---

### Task 9: Carry labels by model and frame

**Files:**
- Modify: `core/faces.h`, `core/faces.cpp` (`carry_labels`, new functions), `core/models.h`, `core/models.cpp:103-112`, `tests/test_faces.cpp` (`CarryLabels` tests at 289-340)

**Interfaces:**
- Consumes: `FaceProvenance`, `LabelledFace` from Task 8.
- Produces:

  ```cpp
  // core/models.h
  inline constexpr const char* kSfaceModelSha256 =
      "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79";
  // core/faces.h
  inline constexpr float kLabelCarryMaxDistance = 1.128f;
  // sha256 of the recognition model file this process loads; computed once.
  const std::string& recognition_model_sha256();
  // L2 distance between the unit-normalized embeddings.
  float normalized_embedding_distance(const std::array<float, 128>& a,
                                      const std::array<float, 128>& b);
  bool same_frame(const FaceProvenance& a, const FaceProvenance& b);
  LabelCarry carry_labels(const std::vector<LabelledFace>& labelled,
                          const std::vector<FaceInfo>& detected,
                          const FaceProvenance& current,
                          float max_distance = kLabelCarryMaxDistance,
                          float min_overlap = kLabelCarryMinOverlap);
  ```

- [ ] **Step 1: Write the failing tests.** Replace the existing `CarryLabels` tests' calls with the new signature. Each existing test passes a `current` whose model differs from the labelled faces' model and whose frame equals theirs, so they keep exercising overlap. Then add:

  ```cpp
  namespace {
  const std::string kModelA(64, 'a'), kModelB(64, 'b');
  FaceProvenance prov(const std::string& model, const std::string& frame_sha, int w, int h) {
      return {"file", w, h, frame_sha, model};
  }
  FaceInfo face_at(int x, int y, int w, int h, float seed) {
      FaceInfo f{};
      for (int i = 0; i < 128; ++i) f.embedding[i] = std::sin(seed * (i + 1));
      f.x = x; f.y = y; f.width = w; f.height = h;
      return f;
  }
  }  // namespace

  TEST(CarryLabels, SameModelMatchesByEmbeddingWhateverTheBoxes) {
      FaceProvenance cur = prov(kModelA, "f2", 320, 240);
      LabelledFace old{face_at(0, 0, 50, 50, 1.0f), 7, prov(kModelA, "f1", 640, 480)};
      FaceInfo moved = face_at(200, 100, 25, 25, 1.0f);  // no overlap, same embedding
      LabelCarry c = carry_labels({old}, {moved}, cur);
      EXPECT_EQ(c.person_ids.at(0), 7);
      EXPECT_EQ(c.dropped, 0u);
  }

  TEST(CarryLabels, SameModelNeverMovesALabelToAnotherPersonsFace) {
      // A frame rotated 180 degrees: the old box of person 7 now sits exactly on
      // person 8's new box. Embeddings decide, so each label stays with its person.
      FaceProvenance cur = prov(kModelA, "rot", 100, 100);
      LabelledFace p7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "orig", 100, 100)};
      LabelledFace p8{face_at(80, 80, 20, 20, 2.0f), 8, prov(kModelA, "orig", 100, 100)};
      FaceInfo new7 = face_at(80, 80, 20, 20, 1.0f);
      FaceInfo new8 = face_at(0, 0, 20, 20, 2.0f);
      LabelCarry c = carry_labels({p7, p8}, {new7, new8}, cur);
      EXPECT_EQ(c.person_ids.at(0), 7);
      EXPECT_EQ(c.person_ids.at(1), 8);
  }

  TEST(CarryLabels, AModelChangeInTheSameFrameMatchesByOverlap) {
      FaceProvenance cur = prov(kModelB, "f", 100, 100);
      LabelledFace old{face_at(10, 10, 30, 30, 1.0f), 7, prov(kModelA, "f", 100, 100)};
      FaceInfo again = face_at(10, 10, 30, 30, 9.0f);  // new model, unrelated embedding
      EXPECT_EQ(carry_labels({old}, {again}, cur).person_ids.at(0), 7);
  }

  TEST(CarryLabels, AModelChangeInASameSizedRotatedFrameDropsRatherThanMoves) {
      // Square frame rotated 90 degrees: same size, different bytes. Under a model
      // change nothing can match safely, so both labels drop.
      FaceProvenance cur = prov(kModelB, "rot90", 100, 100);
      LabelledFace p7{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "orig", 100, 100)};
      LabelledFace p8{face_at(80, 0, 20, 20, 2.0f), 8, prov(kModelA, "orig", 100, 100)};
      FaceInfo a = face_at(80, 0, 20, 20, 5.0f);  // where p7's old box was rotated onto p8's
      FaceInfo b = face_at(80, 80, 20, 20, 6.0f);
      LabelCarry c = carry_labels({p7, p8}, {a, b}, cur);
      EXPECT_EQ(c.person_ids.at(0), -1);
      EXPECT_EQ(c.person_ids.at(1), -1);
      EXPECT_EQ(c.dropped, 2u);
  }

  TEST(CarryLabels, ARowFromBeforeV10IsTakenAsTheSfaceModelWithAnUnknownFrame) {
      FaceProvenance legacy{};  // what a pre-v10 row reads back as
      LabelledFace old{face_at(0, 0, 20, 20, 1.0f), 7, legacy};
      FaceInfo same = face_at(50, 50, 20, 20, 1.0f);
      EXPECT_EQ(carry_labels({old}, {same}, prov(kSfaceModelSha256, "f", 100, 100))
                    .person_ids.at(0),
                7)
          << "same model as the only one phig shipped: matched by embedding";
      FaceInfo overlapping = face_at(0, 0, 20, 20, 9.0f);
      EXPECT_EQ(carry_labels({old}, {overlapping}, prov(kModelB, "f", 100, 100)).dropped, 1u)
          << "a different model with an unknown frame cannot use overlap";
  }

  TEST(CarryLabels, AnEmbeddingBeyondTheThresholdDoesNotMatch) {
      FaceProvenance cur = prov(kModelA, "f", 100, 100);
      LabelledFace old{face_at(0, 0, 20, 20, 1.0f), 7, prov(kModelA, "f", 100, 100)};
      FaceInfo stranger = face_at(0, 0, 20, 20, 1.0f);
      for (int i = 0; i < 128; ++i) stranger.embedding[i] = -stranger.embedding[i];  // distance 2
      EXPECT_EQ(carry_labels({old}, {stranger}, cur).dropped, 1u);
  }

  TEST(SameFrame, NeedsAKnownHashAndTheSameSize) {
      EXPECT_TRUE(same_frame(prov("", "h", 10, 20), prov("", "h", 10, 20)));
      EXPECT_FALSE(same_frame(prov("", "h", 10, 20), prov("", "h", 20, 10)));
      EXPECT_FALSE(same_frame(prov("", "h", 10, 20), prov("", "g", 10, 20)));
      EXPECT_FALSE(same_frame(prov("", "", 10, 20), prov("", "", 10, 20)));
  }
  ```

  Also a test that `normalized_embedding_distance` of a vector with itself is 0, with its negation is 2, and does not depend on scaling either vector.

- [ ] **Step 2: Run to verify failure.** The build fails: `carry_labels` has no 3-argument form.

- [ ] **Step 3: Implement.**
  - `models.cpp`: use `kSfaceModelSha256` in the download list instead of the literal.
  - `recognition_model_sha256()`: a function-local static initialized from `compute_sha256(sface_model_path())`. It throws on failure, as `compute_sha256` does. It is called only after the models are known to exist.
  - `normalized_embedding_distance`: divide each vector by its L2 norm (a zero norm gives distance 2, which never matches), then Euclidean distance.
  - `same_frame`: `!a.frame_sha256.empty() && a.frame_sha256 == b.frame_sha256 && a.frame_width == b.frame_width && a.frame_height == b.frame_height`.
  - `carry_labels`:
    - For each labelled face, its model is `provenance.model_sha256`, or `kSfaceModelSha256` when empty.
    - If that model equals `current.model_sha256`, add an embedding pair for every detection within `max_distance`.
    - Otherwise, if `same_frame(provenance, current)`, add an overlap pair for every detection at or above `min_overlap`.
    - Otherwise add nothing.
    - Assign embedding pairs first, ascending by distance, then overlap pairs, descending by overlap. Both passes are greedy and one-to-one, sharing the taken flags, with `std::stable_sort` so a run is repeatable. Count the untaken labelled faces as `dropped`.
  - Update the comments on `kLabelCarryMinOverlap` and `carry_labels` in `faces.h` to describe this rule, without citing the old measurement.
  - `FacesEngine::run` still calls the 2-argument form. Make it compile by passing `FaceProvenance{}` as `current` for now; Task 10 fills it.

- [ ] **Step 4: Run** `./build/phig-tests --gtest_filter='CarryLabels.*:SameFrame.*:*Embedding*'`, then the full suite.

- [ ] **Step 5: Commit.**

  ```bash
  git add core/faces.h core/faces.cpp core/models.h core/models.cpp core/faces_engine.cpp tests/test_faces.cpp
  git commit -m "Carry face labels by embedding, and by overlap only in a recorded identical frame"
  ```

---

### Task 10: Every face writer records provenance

**Files:**
- Modify: `core/faces_engine.cpp:141-205`, `core/scan_engine.h:48-51`, `core/scan_engine.cpp` (`process_file` at 29-96, batch insert at 246-262, faces-only path at 360-385), `tests/test_faces_engine.cpp`, `tests/test_scan_engine.cpp`

**Interfaces:**
- Consumes: `decode_image_with_warning(path, hash_source)` (Task 4), `FaceProvenance`, the 3-argument `insert_face` (Task 8), `carry_labels` and `recognition_model_sha256` (Task 9).
- Produces: `ProcessResult` gains `DecodeFrame frame;`. A helper in `core/faces.h`:

  ```cpp
  // The provenance to record for faces detected in `frame` of an image whose
  // own file has sha256 `image_sha256`.
  FaceProvenance face_provenance(const DecodeFrame& frame, const std::string& image_sha256);
  ```

  It returns `{decode_source_name(frame.source), frame.width, frame.height, frame.source_sha256.empty() ? image_sha256 : frame.source_sha256, recognition_model_sha256()}`.

- [ ] **Step 1: Write the failing tests** in `tests/test_faces_engine.cpp`, each starting with `if (auto why = face_path_blocker(); !why.empty()) GTEST_SKIP() << why;`:

  - `RebuildRecordsTheFrameAndModelOnEveryFace`: build a `ScratchFaces coll(1)` and run a rebuild. Then read the faces' provenance columns with `sqlite3` on `coll.db_path` and assert:
    - `frame_source = 'file'`;
    - the frame size equals the fixture's decoded size;
    - `frame_sha256` equals `compute_sha256` of the file;
    - `model_sha256` equals `recognition_model_sha256()`.
  - `RebuildCarriesALabelWhenTheImageIsReplacedByARescaledCopy`:
    1. Rebuild once and name the first face "Ada".
    2. Overwrite the file with `cv::resize` to half size, written with `cv::imwrite`.
    3. Rebuild again.
    4. Assert some face of the image still has Ada's `person_id` and `labels_dropped == 0`.
  - `RebuildCarriesALabelWhenTheImageIsReplacedByACroppedCopy`: the same, cropping 10% off the left and top. The face must remain inside the crop; take a centred face from `faces_large.jpg`.
  - `RebuildDropsALabelFromAnotherModelInAnotherFrame`:
    1. Insert a labelled face with provenance `{"file", 1, 1, "other", std::string(64, 'b')}` and a box overlapping a real detection.
    2. Rebuild.
    3. Assert `labels_dropped == 1` and that the label is not on any face.

  In `tests/test_scan_engine.cpp`, `ScanWithFacesRecordsProvenance`:
  - scan the faces fixture directory with faces on, and assert every face row has a non-NULL `frame_sha256` and `model_sha256`;
  - then delete that image's faces, rescan, which takes the faces-only path, and assert the same.

- [ ] **Step 2: Run to verify failure.** The provenance columns are NULL; the rescaled-copy test drops the label.

- [ ] **Step 3: Implement.**
  - `FacesEngine::run`:
    - Decode with `decode_image_with_warning(img_info.path, /*hash_source=*/true)`.
    - Build `FaceProvenance cur = face_provenance(decoded.frame, img_info.sha256)`.
    - Pass `cur` to `carry_labels` and to every `insert_face`.
    - Call `recognition_model_sha256()` once before the workers start, so a missing model fails before any image is touched.
  - `process_file`:
    - Use `decode_image_with_warning(info.path, /*hash_source=*/face_detector != nullptr)`.
    - Log a non-empty warning as `decode_image` did, then store `decoded.frame` in the result.
  - Batch insert: `db_.insert_face(image_id, face, face_provenance(r.frame, r.info.sha256))`.
  - Faces-only path:
    - Decode with `decode_image_with_warning(canonical, true)` and log the warning as before.
    - Inside the transaction, read `db_.get_sha256(canonical)` and pass `face_provenance(decoded.frame, sha.value_or(""))`.

- [ ] **Step 4: Run** `./build/phig-tests --gtest_filter='FacesEngine*:ScanEngine*'`, then the full suite. Check that the `FacesEngineRun` tests ran rather than skipped: the models must be present, which they are in the test scratch home.

- [ ] **Step 5: Commit.**

  ```bash
  git add core/faces.h core/faces.cpp core/faces_engine.cpp core/scan_engine.h core/scan_engine.cpp tests/test_faces_engine.cpp tests/test_scan_engine.cpp
  git commit -m "Record the frame and model on every face a scan or rebuild writes"
  ```

---

### Task 11: Performance

**Files:**
- Create: `tests/perf_faces.cpp`
- Modify: root `CMakeLists.txt` (add to `phig-perf-tests`, and mark `FacesPerf` `RUN_SERIAL` as `DuplicatesPerf` is), `plans/060-silent-loss-on-success.md` (record)

- [ ] **Step 1: Write the perf test.**

  `FacesPerf.AFrameChangeDoesNotSlowARebuild` builds 40 images, alternating `faces_small.jpg` and `faces_large.jpg`, scans them with faces, names every detected face, and times one rebuild. It then rewrites each file as a half-size copy and times a second rebuild, in which every label now goes through the changed-frame path. Each rebuild runs three times and the test takes the median. It asserts `changed <= 1.15 * unchanged`, and skips under `face_path_blocker()`.

  The ratio compares two runs of the same binary on the same machine, the same way `DuplicatesPerf` avoids depending on machine speed. The changed set is smaller in pixels, so it should be faster, not slower. That direction is deliberate: it bounds the matching overhead from above.

- [ ] **Step 2: Run** `./build/phig-perf-tests --gtest_filter='FacesPerf.*'`. Expected: pass, printing both medians.

- [ ] **Step 3: Before-and-after measurement.**
  1. Build Release at the branch's base and at `HEAD`.
  2. Over 200 faces fixtures copied to tmpfs, under the scratch-HOME exports, time `phig scan --faces` and `phig face rebuild`, three runs each, and take the medians.
  3. Record the four numbers, the machine and both commits in plan 060's open question 7 section under "Performance".
  4. If either command is more than 10% slower at `HEAD`, stop and report to the controller.

- [ ] **Step 4: Commit.**

  ```bash
  git add tests/perf_faces.cpp CMakeLists.txt plans/060-silent-loss-on-success.md
  git commit -m "Measure label carry's cost on a face rebuild"
  ```

---

### Task 12: Rules, comments and plan status

**Files:**
- Modify: `rules/authorization.toml` (R60), `rules/metadata.toml` (new R86, after R85), `rules/by-*.md` (generated), `core/faces.h` (any remaining comment that cites the 564-face measurement), `plans/060-silent-loss-on-success.md` (Progress)

- [ ] **Step 1: Update R60.**
  - **rationale:** replace the sentences about box overlap and the measured exact reproduction with how matching works now:
    - by embedding when the recognition model is unchanged;
    - by overlap when the model changed and the recorded frame is identical;
    - otherwise the label is dropped and counted.
  - **enforced_at:** add `core/database.cpp:insert_face`.
  - **tests:** add `FacesEngineRun.RebuildCarriesALabelWhenTheImageIsReplacedByARescaledCopy` and `CarryLabels.ARowFromBeforeV10IsTakenAsTheSfaceModelWithAnUnknownFrame`.
  - **test_gap:** rewrite it. The engine witnesses still skip without the face path, and authorization before a drop is still unpinned. The frame-change case is now covered.
  - **status:** stays `partial`.

- [ ] **Step 2: Add R86** to `rules/metadata.toml`:

  ```toml
  [[rule]]
  id = "R86"
  tier = "engine"
  concern = "metadata"
  statement = "A person label carried across a face rebuild lands on a face of the same person, or is dropped and counted; it never moves to another person's face."
  rationale = "A rebuild re-detects faces and carries each image's labels onto the new detections. Box positions depend on the decoded frame, which can change without changing its size — a square image rotated 90 degrees, any image rotated 180 degrees or mirrored — so a box match can attach a name to the wrong person with no signal. carry_labels therefore matches by embedding when the recognition model is unchanged, since embeddings come from aligned face crops and do not depend on the frame, and by box overlap only when the model changed and the face rows record a frame identical to the new decode (same bytes by sha256, same size). Anything else is dropped and counted in labels_dropped. A wrong label is worse than a dropped one: a drop is reported, a wrong label is not."
  enforced_at = [
      "core/faces.cpp:carry_labels",
      "core/faces.cpp:same_frame",
      "core/faces_engine.cpp:FacesEngine::run",
  ]
  binds = ["faces.rebuild"]
  protects = ["user-metadata"]
  tests = [
      "CarryLabels.SameModelNeverMovesALabelToAnotherPersonsFace",
      "CarryLabels.AModelChangeInASameSizedRotatedFrameDropsRatherThanMoves",
      "CarryLabels.ARowFromBeforeV10IsTakenAsTheSfaceModelWithAnUnknownFrame",
      "FacesEngineRun.RebuildDropsALabelFromAnotherModelInAnotherFrame",
  ]
  test_kind = "witness"
  status = "holds"
  test_gap = "The rotated-frame cases are unit tests over synthetic embeddings; no engine test rotates a real image, because the detector does not find faces turned 90 degrees reliably. The embedding threshold is SFace's published value, not one measured on phig's data."
  ```

  Check the field names, `concern` value and `protects` id against `rules/README.md` and `rules/taxonomy.toml`, and fix any that differ.

- [ ] **Step 3: Regenerate and check.**

  ```bash
  python3 scripts/rules.py --check --emit
  ./build/phig-tests --gtest_output=xml:/tmp/phig-rules.xml
  python3 scripts/rules.py --check --test-results /tmp/phig-rules.xml
  ```

  Expected: both report OK.

- [ ] **Step 4: Plan 060 Progress.** Replace "the open question 7 decision, which is designed and not yet built" with what shipped, listing this branch's commits.

- [ ] **Step 5: Commit.**

  ```bash
  git add rules/ core/faces.h plans/060-silent-loss-on-success.md
  git commit -m "Record R86 and update R60 for frame-aware label carry"
  ```

---

## Self-review notes

- **Spec coverage, plan 069.** Steps 0-1 → Task 1, step 2 → Task 2, step 3 → Task 3, step 4 → Task 4, step 5 → Task 5, step 6 → Task 6, steps 7-8 → Task 7. Decision 5 (build and packages) → Task 2. The frame in `DecodeResult` → Tasks 2 and 4.
- **Spec coverage, plan 060's open question 7 decision.**
  - Points 1, 5 and 6 → Tasks 8 and 9.
  - Points 2-4 and 7 → Task 9.
  - Point 8 (R86) → Task 12.
  - "Every path that writes faces" → Task 10.
  - The testing list → Tasks 8-10.
  - Performance → Task 11.
- **Not covered, by design.** Turning the embedded preview upright, a separate finding. Verifying reason 2's orientation claim, which needs a RAW fixture. Task 1's raw.pixls.us files make it possible to check by hand, and the result belongs to that finding.
