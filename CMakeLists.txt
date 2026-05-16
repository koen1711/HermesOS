cmake_minimum_required(VERSION 3.5.0)
project(OS LANGUAGES C ASM)



set(OS_TARGET_PLATFORM "x86_64")

file(GLOB_RECURSE SOURCES "src/kernel/*.h" "src/kernel/*.c" )
file(GLOB_RECURSE ALL_ASM_SOURCES "src/*.asm")
list(APPEND SOURCES ${ALL_ASM_SOURCES})
set_source_files_properties(${ALL_ASM_SOURCES} PROPERTIES LANGUAGE ASM)

# If ASM compiler is a GCC driver (not raw 'as'), it needs -x assembler for .asm files
get_filename_component(_asm_name "${CMAKE_ASM_COMPILER}" NAME)
if(NOT _asm_name MATCHES "-as$" AND NOT _asm_name STREQUAL "as")
    set_source_files_properties(${ALL_ASM_SOURCES} PROPERTIES
        COMPILE_FLAGS "-x assembler")
endif()

# Split CMAKE_BINARY_DIR into everything after the last /
# Find the last occurrence of "/"
string(FIND ${CMAKE_BINARY_DIR} "/" LAST_SLASH REVERSE)
string(SUBSTRING ${CMAKE_BINARY_DIR} ${LAST_SLASH} -1 CMAKE_BINARY_DIR_NAME)
string(REGEX REPLACE "^/" "" CMAKE_BINARY_DIR_NAME "${CMAKE_BINARY_DIR_NAME}")

# Run the code at assets/compile_pci_ids.py to generate pci_ids.c
add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/pci_ids.c
        COMMAND python3 ${CMAKE_SOURCE_DIR}/assets/compile_pci_ids.py
        ARGS ${CMAKE_SOURCE_DIR}/assets/pci.ids ${CMAKE_BINARY_DIR}/
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/compile_pci_ids.py
        COMMENT "Generating pci_ids.c"
)

list(APPEND SOURCES ${CMAKE_BINARY_DIR}/pci_ids.c)

add_executable(${PROJECT_NAME}
        ${SOURCES}
)
include_directories(src/kernel)
include_directories(include)

# Freestanding kernel compile/link flags (C only — as doesn't understand GCC flags)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-ffreestanding -fno-builtin -nostdinc
    -fno-pie -mno-red-zone -mno-sse>)
target_link_options(${PROJECT_NAME} PRIVATE
    -nostdlib -no-pie -static -z max-page-size=0x1000)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/isodir/boot/grub
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/root_dir ${CMAKE_BINARY_DIR}/isodir/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${PROJECT_NAME} ${CMAKE_BINARY_DIR}/isodir/boot/${PROJECT_NAME}
        COMMENT "Generating your .iso file"
)

add_subdirectory(src)
