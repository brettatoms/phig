# ---- libphig: C ABI shared library ----
add_library(phig_abi SHARED
    context.cpp
    query.cpp
    records.cpp
    thumbs.cpp
    mutate.cpp
    operations.cpp
    organize.cpp
    settings.cpp
)
set_target_properties(phig_abi PROPERTIES
    OUTPUT_NAME phig
    # Must track PHIG_ABI_VERSION_MAJOR.MINOR in libphig/include/phig/phig.h.
    # SOVERSION is the major alone, so an additive minor bump keeps the soname.
    # Pre-release, neither moves for a breaking change — see the header for why.
    VERSION 1.5.0
    SOVERSION 1
    # Hide everything not explicitly exported so the ABI surface is exactly
    # what the header declares.
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_compile_options(phig_abi PRIVATE ${PHIG_WARNINGS})
target_include_directories(phig_abi
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
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
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/phig.exported_symbols")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(phig_abi PRIVATE
        "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/phig.version")
endif()

install(TARGETS phig_abi LIBRARY DESTINATION lib)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/phig DESTINATION include)
