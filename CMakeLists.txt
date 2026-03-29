cmake_minimum_required(VERSION 3.20)
project(phig LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc img_hash objdetect)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXIF REQUIRED libexif)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

# ---- Shared library (compiled once, used by binary + tests) ----
add_library(phig-lib STATIC
    src/scanner.cpp
    src/hasher.cpp
    src/exif.cpp
    src/database.cpp
    src/filters.cpp
    src/faces.cpp
    src/models.cpp
    vendor/sqlite-vec/sqlite-vec.c
)

target_include_directories(phig-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${EXIF_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/vendor/sqlite-vec
)

target_link_libraries(phig-lib PUBLIC
    ${OpenCV_LIBS}
    ${EXIF_LIBRARIES}
    SQLite::SQLite3
    CURL::libcurl
    pthread
)

target_compile_definitions(phig-lib PUBLIC SQLITE_VEC_STATIC SQLITE_CORE)

# ---- Binary ----
add_executable(phig src/main.cpp)
target_link_libraries(phig PRIVATE phig-lib)
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
)

target_link_libraries(phig-tests PRIVATE
    phig-lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(phig-tests)
