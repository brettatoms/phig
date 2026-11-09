cmake_minimum_required(VERSION 3.20)
project(phig VERSION 0.3.0 LANGUAGES C CXX)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cli/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags applied to first-party code only (not vendored). CI sets
# -DPHIG_WERROR=ON to make warnings fatal; local dev builds stay lenient.
option(PHIG_WERROR "Treat compiler warnings as errors" OFF)
set(PHIG_WARNINGS -Wall -Wextra)
if(PHIG_WERROR)
    list(APPEND PHIG_WARNINGS -Werror)
endif()

# Version floors. Each is picked from what a mainstream distro ships (Debian
# 12 "bookworm"), not from this machine's Arch versions, so configure fails
# cleanly on something too old rather than misbehaving at runtime. Where a
# floor isn't justified by a known API dependency, the call stays bare
# REQUIRED — see the comment on each.
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc objdetect videoio)
# OpenCV's own OpenCVConfig-version.cmake only accepts a requested version
# whose major matches its own (so find_package(OpenCV 4.5.4) would reject 5.x,
# not just pre-4.5.4) — the floor has to be checked manually instead.
if(OpenCV_VERSION VERSION_LESS 4.5.4)
    message(FATAL_ERROR "phig requires OpenCV >= 4.5.4 (found ${OpenCV_VERSION}): "
                         "core/faces.cpp calls cv::FaceDetectorYN::create and "
                         "cv::FaceRecognizerSF::create unconditionally, and both need "
                         "4.5.4. (cv::imread has applied EXIF orientation by default "
                         "since 3.4, which also depends on this floor.)")
endif()
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
# 0.6.24: Debian bookworm's libexif-dev. No known dependency on anything newer.
pkg_check_modules(EXIF REQUIRED libexif>=0.6.24)
# 3.40: Debian bookworm's libsqlite3-dev. WAL mode and BEGIN IMMEDIATE predate this by years.
find_package(SQLite3 3.40 REQUIRED)
# 7.88: Debian bookworm's libcurl4-openssl-dev. Only curl_easy_* calls are
# used (core/), all present since curl's early releases; the floor tracks the
# distro baseline rather than any specific API requirement.
find_package(CURL 7.88 REQUIRED)
# 1.10: Debian bookworm's libspdlog-dev. custom_flag_formatter and
# pattern_time_type::utc (core/log.cpp) both predate 1.10 by several releases.
find_package(spdlog 1.10 REQUIRED)
# 0.20: Debian bookworm's libraw-dev.
pkg_check_modules(LIBRAW_R REQUIRED libraw_r>=0.20)
# 1.16: Debian bookworm's libheif-dev. Decodes HEIC/HEIF and AVIF, which
# OpenCV's imgcodecs does not, and exposes the container's EXIF block. The
# calls used (heif_decode_image, heif_image_handle_get_metadata) predate this
# version by several releases; the floor tracks the distro baseline.
pkg_check_modules(LIBHEIF REQUIRED libheif>=1.16)
# 0.7: Debian bookworm's libjxl-dev. Decodes JPEG XL, which libheif does not
# handle at all — HEIF-family containers only. The box API used for the Exif
# payload (JxlDecoderSetDecompressBoxes) landed in 0.7.
pkg_check_modules(LIBJXL REQUIRED libjxl>=0.7)
# FFmpeg 5.1 (Debian bookworm) soname majors: avformat/avcodec 59, avutil 57,
# swscale 6. core/metadata.cpp uses AVCodecParameters (FFmpeg 3.1+) and
# av_display_rotation_get (FFmpeg 2.x+) — both far older than bookworm's
# version, so bookworm's own shipped version is the binding constraint, not
# a specific API. swscale has no call sites in core/ (linked transitively);
# floored at the same distro baseline for consistency.
pkg_check_modules(AVFORMAT REQUIRED libavformat>=59)
pkg_check_modules(AVCODEC REQUIRED libavcodec>=59)
pkg_check_modules(AVUTIL REQUIRED libavutil>=57)
pkg_check_modules(SWSCALE REQUIRED libswscale>=6)

# ---- Vendored third-party code (built without our strict warnings) ----
# inih is packaged on most distros (Arch core/libinih, Debian/Ubuntu
# libinih-dev), so a packager will object to phig bundling a copy. Default
# OFF so nothing changes for existing developers.
option(PHIG_USE_SYSTEM_INIH "Link the system inih instead of the vendored copy" OFF)

set(PHIG_VENDOR_SOURCES vendor/sqlite-vec/sqlite-vec.c)
if(NOT PHIG_USE_SYSTEM_INIH)
    list(APPEND PHIG_VENDOR_SOURCES vendor/inih/ini.c vendor/inih/INIReader.cpp)
endif()

add_library(phig-vendor STATIC ${PHIG_VENDOR_SOURCES})
target_include_directories(phig-vendor PUBLIC
    ${CMAKE_SOURCE_DIR}/vendor/sqlite-vec
)
target_compile_definitions(phig-vendor PUBLIC SQLITE_VEC_STATIC SQLITE_CORE)
set_target_properties(phig-vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PHIG_USE_SYSTEM_INIH)
    # Separate .pc files for the C parser and its C++ wrapper (core/config.cpp
    # includes INIReader.h) — confirmed present as "inih" and "INIReader" on
    # this system, both tracking version 62.
    pkg_check_modules(INIH REQUIRED inih)
    pkg_check_modules(INIREADER REQUIRED INIReader)
    target_include_directories(phig-vendor PUBLIC ${INIH_INCLUDE_DIRS} ${INIREADER_INCLUDE_DIRS})
    target_link_libraries(phig-vendor PUBLIC ${INIH_LIBRARIES} ${INIREADER_LIBRARIES})
    target_link_directories(phig-vendor PUBLIC ${INIH_LIBRARY_DIRS} ${INIREADER_LIBRARY_DIRS})
else()
    target_include_directories(phig-vendor PUBLIC ${CMAKE_SOURCE_DIR}/vendor/inih)
endif()

add_subdirectory(core)
add_subdirectory(libphig)
add_subdirectory(cli)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/phig.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/phig.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/phig.pc DESTINATION lib/pkgconfig)

# ---- Tests ----
enable_testing()
# 1.12: Debian bookworm's libgtest-dev. No version-specific gtest API in use.
find_package(GTest 1.12 REQUIRED)

add_executable(phig-tests
    tests/scratch_home.cpp
    tests/test_scanner.cpp
    tests/test_hasher.cpp
    tests/test_jpeg_decode.cpp
    tests/test_image_codec.cpp
    tests/test_live_photo.cpp
    tests/test_exif.cpp
    tests/test_database.cpp
    tests/test_organize.cpp
    tests/test_faces.cpp
    tests/test_porcelain.cpp
    tests/test_thumbs.cpp
    tests/test_video.cpp
    tests/test_paths.cpp
    tests/test_config.cpp
    tests/test_settings.cpp
    tests/test_organize_exec.cpp
    tests/test_organize_engine.cpp
    tests/test_libphig_organize.cpp
    tests/test_remove_exec.cpp
    tests/test_relink.cpp
    tests/test_log.cpp
    tests/test_sidecar.cpp
    tests/test_dates.cpp
    tests/test_dates_format.cpp
    tests/test_duplicates.cpp
    tests/test_raw.cpp
    tests/test_lockfile.cpp
    tests/test_catalog.cpp
    tests/test_cli_output.cpp
    tests/test_parallel.cpp
    tests/test_progress_queue.cpp
    tests/test_scan_engine.cpp
    tests/test_thumbs_engine.cpp
    tests/test_faces_engine.cpp
    tests/test_faces_identify.cpp
    tests/test_search_engine.cpp
    tests/test_libphig_query.cpp
    tests/test_libphig_abi.cpp
    tests/test_libphig_mutate.cpp
    tests/test_libphig_operations.cpp
    tests/test_libphig_settings.cpp
    tests/test_libphig_c_compile.c
)
# Applies to phig-tests' C sources only (test_libphig_c_compile.c) — proves
# include/phig/phig.h is valid C99, not just whatever gnu17 happens to allow.
set_target_properties(phig-tests PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)

target_link_libraries(phig-tests PRIVATE
    phig-core
    phig_abi
    GTest::gtest_main
)

include(GoogleTest)
# Run tests from the source root so fixtures that reference tests/data/ by
# relative path resolve regardless of how the suite is launched (e.g. ctest).
gtest_discover_tests(phig-tests
    DISCOVERY_TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ---- Performance tests ----
# Kept out of phig-tests: each of these times an O(n^2) brute-force baseline
# alongside the code under test, which is what removed their dependence on
# absolute machine speed (see tests/perf_duplicates.cpp) but also makes them
# dominate phig-tests' runtime if left in the default suite. Still built and run
# by ctest, so CI keeps gating on them; just not part of the fast local loop.
add_executable(phig-perf-tests
    tests/scratch_home.cpp
    tests/perf_duplicates.cpp
    tests/perf_faces.cpp
)
target_link_libraries(phig-perf-tests PRIVATE
    phig-core
    phig_abi
    GTest::gtest_main
)
# RUN_SERIAL because a timing assertion is only meaningful on a machine the test
# is not sharing. Timing the oracle in the same process removes the dependence
# on how fast the machine is, but not on how loaded it is: the banded index
# builds hash tables and is memory-bound, the brute-force baseline is a tight
# loop over a contiguous vector and is compute-bound, so concurrent work slows
# them by different factors and moves the ratio the assertion is made against.
# Measured on the 8-centre/12k-image corpus: idle the ratio sits at 0.94-1.04,
# under `ctest --parallel` it drifts to 1.18-1.34, and an observed failure hit
# 1.66 against a 1.50 ceiling — ClusteredCorporaDoNotRegressAtTheDefaultThreshold
# failed 2 of 6 parallel runs and 0 of 15 sequential ones. Serial: 0 of 10.
#
# It is not free. `ctest --parallel` goes from ~14.4s to ~19.3s here, about 35%,
# because ctest drains in-flight tests before a RUN_SERIAL one and starts
# nothing else until it finishes — more than the perf suite's own ~2.2s.
gtest_discover_tests(phig-perf-tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES RUN_SERIAL TRUE
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ---- GTK client ----
# Off by default, so a plain build needs neither gtk4 nor libadwaita. With it
# on, `cmake --build build --target phig-gtk` builds the client and the phig
# libraries it links, and nothing else.
option(PHIG_BUILD_GTK "Build the GTK4/libadwaita client in gtk/" OFF)
if(PHIG_BUILD_GTK)
    add_subdirectory(gtk)
endif()
