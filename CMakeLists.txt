cmake_minimum_required(VERSION 3.20)
project(phig VERSION 0.3.0 LANGUAGES C CXX)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cli/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
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

find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc objdetect videoio)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXIF REQUIRED libexif)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)
find_package(spdlog REQUIRED)
pkg_check_modules(LIBRAW_R REQUIRED libraw_r)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)

# ---- Vendored third-party code (built without our strict warnings) ----
add_library(phig-vendor STATIC
    vendor/sqlite-vec/sqlite-vec.c
    vendor/inih/ini.c
    vendor/inih/INIReader.cpp
)
target_include_directories(phig-vendor PUBLIC
    ${CMAKE_SOURCE_DIR}/vendor/sqlite-vec
    ${CMAKE_SOURCE_DIR}/vendor/inih
)
target_compile_definitions(phig-vendor PUBLIC SQLITE_VEC_STATIC SQLITE_CORE)
set_target_properties(phig-vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(core)
add_subdirectory(libphig)
add_subdirectory(cli)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/phig.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/phig.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/phig.pc DESTINATION lib/pkgconfig)

# ---- Tests ----
enable_testing()
find_package(GTest REQUIRED)

add_executable(phig-tests
    tests/test_scanner.cpp
    tests/test_hasher.cpp
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
    tests/test_remove_exec.cpp
    tests/test_relink.cpp
    tests/test_log.cpp
    tests/test_sidecar.cpp
    tests/test_dates.cpp
    tests/test_dates_format.cpp
    tests/test_duplicates.cpp
    tests/test_raw.cpp
    tests/test_lockfile.cpp
    tests/test_parallel.cpp
    tests/test_progress_queue.cpp
    tests/test_scan_engine.cpp
    tests/test_thumbs_engine.cpp
    tests/test_faces_engine.cpp
    tests/test_search_engine.cpp
    tests/test_libphig_query.cpp
    tests/test_libphig_abi.cpp
    tests/test_libphig_mutate.cpp
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
# alongside the code under test, which is what removed their flakiness (see
# tests/perf_duplicates.cpp) but also makes them dominate phig-tests' runtime
# if left in the default suite. Still built and run by ctest, so CI keeps
# gating on them; just not part of the fast local loop.
add_executable(phig-perf-tests
    tests/perf_duplicates.cpp
)
target_link_libraries(phig-perf-tests PRIVATE
    phig-core
    phig_abi
    GTest::gtest_main
)
gtest_discover_tests(phig-perf-tests
    DISCOVERY_TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
