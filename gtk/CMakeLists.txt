cmake_minimum_required(VERSION 3.20)

if(NOT TARGET phig-core)
    message(FATAL_ERROR "gtk/ is configured from the repository root: "
                        "cmake -B build -DPHIG_BUILD_GTK=ON")
endif()

project(phig-gtk VERSION 0.1.0 LANGUAGES CXX)

set(PHIG_GTK_WARNINGS -Wall -Wextra)

pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 libadwaita-1)

add_executable(phig-gtk src/main.cpp src/gallery-model.cpp src/gallery-selection.cpp
               src/thumb-loader.cpp src/collection.cpp
               src/startup.cpp src/browse.cpp src/viewer.cpp src/scan-ui.cpp src/query.cpp
               src/format.cpp src/scan-watch.cpp)
target_compile_options(phig-gtk PRIVATE ${PHIG_GTK_WARNINGS})
target_link_libraries(phig-gtk PRIVATE phig-core PkgConfig::GTK)

# `--target gtk-all` builds the client and every gtk test binary.
add_custom_target(gtk-all)
add_dependencies(gtk-all phig-gtk)

# One test binary per source under test, labelled `gtk` so CI and
# `ctest -L gtk` select them without naming each one.
function(phig_gtk_test name)
    add_executable(test-${name} ${ARGN})
    target_compile_options(test-${name} PRIVATE ${PHIG_GTK_WARNINGS})
    target_include_directories(test-${name} PRIVATE src tests)
    target_link_libraries(test-${name} PRIVATE PkgConfig::GTK)
    add_test(NAME ${name} COMMAND test-${name})
    set_tests_properties(${name} PROPERTIES LABELS gtk)
    add_dependencies(gtk-all test-${name})
endfunction()

# Linked against tests/fake-collection.cpp in place of src/collection.cpp, so
# it needs no collection and no phig-core.
phig_gtk_test(gallery-model tests/test-gallery-model.cpp src/gallery-model.cpp
              tests/fake-collection.cpp)

phig_gtk_test(gallery-selection tests/test-gallery-selection.cpp src/gallery-selection.cpp
              src/gallery-model.cpp tests/fake-collection.cpp)

phig_gtk_test(thumb-loader tests/test-thumb-loader.cpp src/thumb-loader.cpp
              tests/fake-collection.cpp)

phig_gtk_test(connect tests/test-connect.cpp)
phig_gtk_test(background tests/test-background.cpp)
phig_gtk_test(query tests/test-query.cpp src/query.cpp)
phig_gtk_test(format tests/test-format.cpp src/format.cpp)
phig_gtk_test(scan-watch tests/test-scan-watch.cpp src/scan-watch.cpp tests/fake-collection.cpp)
phig_gtk_test(startup tests/test-startup.cpp src/startup.cpp tests/fake-collection.cpp)

# Tests collection.cpp itself, against a database it builds, so unlike the
# other gtk tests it links phig-core.
phig_gtk_test(collection tests/test-collection.cpp src/collection.cpp)
target_link_libraries(test-collection PRIVATE phig-core)
target_include_directories(test-collection PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(test-collection PRIVATE PHIG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
