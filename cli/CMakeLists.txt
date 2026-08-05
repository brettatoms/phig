# ---- Binary ----
add_executable(phig main.cpp)
set_target_properties(phig PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
target_link_libraries(phig PRIVATE phig-core)
target_compile_options(phig PRIVATE ${PHIG_WARNINGS})
install(TARGETS phig DESTINATION bin)
