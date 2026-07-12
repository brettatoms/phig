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

# ---- Shared library (compiled once, used by binary + tests) ----
add_library(phig-lib STATIC
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
    src/organize_exec.cpp
    src/remove_exec.cpp
    src/relink.cpp
    src/log.cpp
    src/sidecar.cpp
    src/dates.cpp
    src/raw.cpp
)

target_include_directories(phig-lib PUBLIC
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

target_link_libraries(phig-lib PUBLIC
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
target_link_directories(phig-lib PUBLIC
    ${EXIF_LIBRARY_DIRS}
    ${LIBRAW_R_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
)

target_compile_options(phig-lib PRIVATE ${PHIG_WARNINGS})

# ---- Binary ----
add_executable(phig src/main.cpp)
target_link_libraries(phig PRIVATE phig-lib)
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
    tests/test_organize_exec.cpp
    tests/test_remove_exec.cpp
    tests/test_relink.cpp
    tests/test_log.cpp
    tests/test_sidecar.cpp
    tests/test_dates.cpp
    tests/test_raw.cpp
)

target_link_libraries(phig-tests PRIVATE
    phig-lib
    GTest::gtest_main
)

include(GoogleTest)
# Run tests from the source root so fixtures that reference tests/data/ by
# relative path resolve regardless of how the suite is launched (e.g. ctest).
gtest_discover_tests(phig-tests
    DISCOVERY_TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
