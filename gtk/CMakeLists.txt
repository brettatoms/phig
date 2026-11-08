cmake_minimum_required(VERSION 3.20)

if(NOT TARGET phig-core)
    message(FATAL_ERROR "gtk/ is configured from the repository root: "
                        "cmake -B build -DPHIG_BUILD_GTK=ON")
endif()

project(phig-gtk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PHIG_GTK_WARNINGS -Wall -Wextra)

pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4 libadwaita-1)

add_executable(phig-gtk src/main.c src/gallery-model.c src/thumb-loader.c)
target_compile_options(phig-gtk PRIVATE ${PHIG_GTK_WARNINGS})
target_link_libraries(phig-gtk PRIVATE phig_abi PkgConfig::GTK)

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

phig_gtk_test(gallery-model tests/test-gallery-model.c src/gallery-model.c)
target_link_libraries(test-gallery-model PRIVATE phig_abi)
set_tests_properties(gallery-model PROPERTIES SKIP_RETURN_CODE 77)

phig_gtk_test(thumb-loader tests/test-thumb-loader.c src/thumb-loader.c)
target_link_libraries(test-thumb-loader PRIVATE phig_abi)
set_tests_properties(thumb-loader PROPERTIES SKIP_RETURN_CODE 77)
