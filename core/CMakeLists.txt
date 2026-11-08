# ---- Shared library (compiled once, used by binary + tests) ----
add_library(phig-core STATIC
    scanner.cpp
    hasher.cpp
    exif.cpp
    database.cpp
    selection.cpp
    faces.cpp
    models.cpp
    thumbs.cpp
    metadata.cpp
    paths.cpp
    catalog.cpp
    config.cpp
    settings.cpp
    organize_engine.cpp
    organize_exec.cpp
    organize_plan.cpp
    remove_exec.cpp
    relink.cpp
    log.cpp
    sidecar.cpp
    dates.cpp
    duplicates.cpp
    raw.cpp
    image_codec.cpp
    live_photo.cpp
    image_codec_libheif.cpp
    image_codec_exif.cpp
    image_codec_libjxl.cpp
    lockfile.cpp
    parallel.cpp
    scan_engine.cpp
    thumbs_engine.cpp
    faces_engine.cpp
    search_engine.cpp
)

set_target_properties(phig-core PROPERTIES
    OUTPUT_NAME phig-core
    # phig-core is STATIC; linking it into a shared library requires
    # position-independent code, which a static target does not get by default.
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(phig-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
    ${EXIF_INCLUDE_DIRS}
    ${LIBRAW_R_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${CMAKE_BINARY_DIR}
)

# PRIVATE, unlike every other dependency here: only image_codec_libheif.cpp
# includes a libheif header, and libheif's pkg-config drags in its own codec
# dependencies' include dirs — one of which (libvmaf) ships a `version.h` that
# shadows phig's generated one, breaking `#include "version.h"` in cli/main.cpp.
# Keeping these off the public interface stops that leak at the source. The
# libraries stay PUBLIC below: phig-core is STATIC, so consumers still resolve
# its libheif symbols at final link.
target_include_directories(phig-core PRIVATE ${LIBHEIF_INCLUDE_DIRS} ${LIBJXL_INCLUDE_DIRS})

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
    ${LIBHEIF_LIBRARIES}
    ${LIBJXL_LIBRARIES}
    pthread
)

# pkg-config libs (libexif, ffmpeg) aren't always in a default linker path —
# e.g. Homebrew's /opt/homebrew/lib on macOS — so add their -L dirs explicitly.
target_link_directories(phig-core PUBLIC
    ${EXIF_LIBRARY_DIRS}
    ${LIBRAW_R_LIBRARY_DIRS}
    ${LIBHEIF_LIBRARY_DIRS}
    ${LIBJXL_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
)

if(APPLE)
    # ImageIO is a C API, so this is plain C++ — no Objective-C++, no .mm file.
    target_sources(phig-core PRIVATE image_codec_apple.cpp)
    target_link_libraries(phig-core PUBLIC "-framework ImageIO" "-framework CoreGraphics"
                                           "-framework CoreFoundation")
endif()

target_compile_options(phig-core PRIVATE ${PHIG_WARNINGS})
