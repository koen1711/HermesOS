add_subdirectory(arch)

target_link_options(${PROJECT_NAME} PRIVATE -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld -z max-page-size=0x1000)