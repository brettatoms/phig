cmake_minimum_required(VERSION 3.20)
project(phig VERSION 0.3.0 LANGUAGES C CXX)

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in
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

# ---- Shared library (compiled once, used by binary + tests) ----
add_library(phig-core STATIC
    src/scanner.cpp
    src/hasher.cpp
    src/exif.cpp
    src/database.cpp
    src/selection.cpp
    src/faces.cpp
    src/models.cpp
    src/thumbs.cpp
    src/metadata.cpp
    src/paths.cpp
    src/config.cpp
    src/settings.cpp
    src/organize_exec.cpp
    src/organize_plan.cpp
    src/remove_exec.cpp
    src/relink.cpp
    src/log.cpp
    src/sidecar.cpp
    src/dates.cpp
    src/duplicates.cpp
    src/raw.cpp
    src/lockfile.cpp
    src/parallel.cpp
    src/scan_engine.cpp
)

set_target_properties(phig-core PROPERTIES
    OUTPUT_NAME phig-core
    # phig-core is STATIC; linking it into a shared library requires
    # position-independent code, which a static target does not get by default.
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(phig-core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${EXIF_INCLUDE_DIRS}
    ${LIBRAW_R_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(phig-core PUBLIC
    phig-vendor
    ${OpenCV_LIBS}
    ${EXIF_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    SQLite::SQLite3
    CURL::libcurl
    spdlog::spdlog
    ${LIBRAW_R_LIBRARIES}
    pthread
)

# pkg-config libs (libexif, ffmpeg) aren't always in a default linker path —
# e.g. Homebrew's /opt/homebrew/lib on macOS — so add their -L dirs explicitly.
target_link_directories(phig-core PUBLIC
    ${EXIF_LIBRARY_DIRS}
    ${LIBRAW_R_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
)

target_compile_options(phig-core PRIVATE ${PHIG_WARNINGS})

# ---- libphig: C ABI shared library ----
add_library(phig_abi SHARED
    src/libphig/context.cpp
    src/libphig/query.cpp
    src/libphig/records.cpp
    src/libphig/thumbs.cpp
    src/libphig/mutate.cpp
    src/libphig/settings.cpp
)
set_target_properties(phig_abi PROPERTIES
    OUTPUT_NAME phig
    # Must track PHIG_ABI_VERSION_MAJOR.MINOR in include/phig/phig.h.
    # SOVERSION is the major alone, so an additive minor bump keeps the soname.
    VERSION 1.1.0
    SOVERSION 1
    # Hide everything not explicitly exported so the ABI surface is exactly
    # what the header declares.
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_compile_options(phig_abi PRIVATE ${PHIG_WARNINGS})
target_include_directories(phig_abi
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# phig-core is the internal static library target (output: libphig-core.a).
target_link_libraries(phig_abi PRIVATE phig-core)
# phig-core and its dependencies (phig-vendor, etc.) are built with default
# symbol visibility for use in the phig/phig-tests executables; without an
# explicit export list, their symbols would leak through into libphig.so's
# exported ABI. Declares exactly the phig_* surface instead of relying on
# hiding everything else, so it stays correct even if a dependency's own
# visibility settings change. ld64 (macOS) and GNU ld/gold (Linux) take this
# via different flags and file formats (Mach-O prefixes C symbols with `_`).
if(APPLE)
    target_link_options(phig_abi PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/libphig/phig.exported_symbols")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(phig_abi PRIVATE
        "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/src/libphig/phig.version")
endif()

install(TARGETS phig_abi LIBRARY DESTINATION lib)
install(DIRECTORY include/phig DESTINATION include)

# ---- Binary ----
add_executable(phig src/main.cpp)
target_link_libraries(phig PRIVATE phig-core)
target_compile_options(phig PRIVATE ${PHIG_WARNINGS})
install(TARGETS phig DESTINATION bin)

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
