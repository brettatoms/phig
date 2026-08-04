cmake_minimum_required(VERSION 3.20)
project(phig-gtk VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PHIG_GTK_WARNINGS -Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PHIG REQUIRED phig)
pkg_check_modules(GTK REQUIRED gtk4 libadwaita-1)

add_executable(phig-gtk src/main.c)
target_compile_options(phig-gtk PRIVATE ${PHIG_GTK_WARNINGS})
target_include_directories(phig-gtk PRIVATE ${PHIG_INCLUDE_DIRS} ${GTK_INCLUDE_DIRS})
target_link_libraries(phig-gtk PRIVATE ${PHIG_LIBRARIES} ${GTK_LIBRARIES})
target_link_directories(phig-gtk PRIVATE ${PHIG_LIBRARY_DIRS} ${GTK_LIBRARY_DIRS})
