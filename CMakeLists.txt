cmake_minimum_required(VERSION 3.20)
project(phig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenCV REQUIRED COMPONENTS core imgcodecs img_hash)
find_package(PkgConfig REQUIRED)
pkg_check_modules(EXIF REQUIRED libexif)
find_package(SQLite3 REQUIRED)

add_executable(phig
    src/main.cpp
    src/scanner.cpp
    src/hasher.cpp
    src/exif.cpp
    src/database.cpp
    src/filters.cpp
)

target_include_directories(phig PRIVATE
    ${OpenCV_INCLUDE_DIRS}
    ${EXIF_INCLUDE_DIRS}
)

target_link_libraries(phig PRIVATE
    ${OpenCV_LIBS}
    ${EXIF_LIBRARIES}
    SQLite::SQLite3
    pthread
)

install(TARGETS phig DESTINATION bin)

# ---- Library target for tests ----
add_library(phig-lib STATIC
    src/scanner.cpp
    src/hasher.cpp
    src/exif.cpp
    src/database.cpp
    src/filters.cpp
)

target_include_directories(phig-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${EXIF_INCLUDE_DIRS}
)

target_link_libraries(phig-lib PUBLIC
    ${OpenCV_LIBS}
    ${EXIF_LIBRARIES}
    SQLite::SQLite3
    pthread
)

# ---- Tests ----
enable_testing()
find_package(GTest REQUIRED)

add_executable(phig-tests
    tests/test_scanner.cpp
    tests/test_hasher.cpp
    tests/test_exif.cpp
    tests/test_database.cpp
    tests/test_organize.cpp
)

target_link_libraries(phig-tests PRIVATE
    phig-lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(phig-tests)
