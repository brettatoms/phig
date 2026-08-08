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
    config.cpp
    settings.cpp
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
