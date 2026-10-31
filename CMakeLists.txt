cmake_minimum_required(VERSION 3.5.0)
project(OS LANGUAGES C ASM)



set(OS_TARGET_PLATFORM "x86_64")

file(GLOB_RECURSE SOURCES "src/kernel/*.h" "src/kernel/*.c" )
file(GLOB_RECURSE ALL_ASM_SOURCES "src/*.asm")
list(APPEND SOURCES ${ALL_ASM_SOURCES})
set_source_files_properties(${ALL_ASM_SOURCES} PROPERTIES LANGUAGE ASM)

# If ASM compiler is a GCC driver (not raw 'as'), it needs -x assembler for .asm files.
# NAME_WE strips the .exe suffix on Windows so the suffix check works there too.
get_filename_component(_asm_name "${CMAKE_ASM_COMPILER}" NAME_WE)
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
# (find_package instead of a bare "python3": on Windows that can resolve to MSYS2's
# python, which can't open C:/ paths)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/pci_ids.c
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/assets/compile_pci_ids.py
        ARGS ${CMAKE_SOURCE_DIR}/assets/pci.ids ${CMAKE_BINARY_DIR}/
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/compile_pci_ids.py
        COMMENT "Generating pci_ids.c"
)

list(APPEND SOURCES ${CMAKE_BINARY_DIR}/pci_ids.c)

add_executable(${PROJECT_NAME}
        ${SOURCES}
        src/kernel/drivers/fs/vfs/vfs.c
        src/kernel/drivers/fs/vfs/vfs.h
        src/kernel/drivers/fs/vfs/driver.h
)
include_directories(src/kernel)
include_directories(include)

# Freestanding kernel compile/link flags (C only — as doesn't understand GCC flags)
target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-m64 -ffreestanding -fno-builtin -nostdinc
    -fno-pie -mno-red-zone -mno-sse>)
target_link_options(${PROJECT_NAME} PRIVATE
    -m64 -nostdlib -no-pie -static -z max-page-size=0x1000)

set(MLIBC_LIB_DIR ${CMAKE_SOURCE_DIR}/buildtools/cross/usr/lib)
set(MLIBC_SHARED_LIBS ld.so libc.so libdl.so libm.so libpthread.so libresolv.so librt.so libssp.so libutil.so)
list(TRANSFORM MLIBC_SHARED_LIBS PREPEND ${MLIBC_LIB_DIR}/)
list(APPEND MLIBC_SHARED_LIBS ${CMAKE_SOURCE_DIR}/buildtools/cross/x86_64-hermes/lib/libgcc_s.so.1
                              ${CMAKE_SOURCE_DIR}/buildtools/cross/x86_64-hermes/lib/libstdc++.so.6)
set(MLIBC_IMAGE_LIBS ${MLIBC_SHARED_LIBS})
list(TRANSFORM MLIBC_IMAGE_LIBS REPLACE "^.*/" ${CMAKE_BINARY_DIR}/isodir/usr/lib/)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/isodir/boot/grub
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/root_dir ${CMAKE_BINARY_DIR}/isodir/
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/isodir/usr/lib
        COMMAND ${CMAKE_COMMAND} -E copy ${MLIBC_SHARED_LIBS} ${CMAKE_BINARY_DIR}/isodir/usr/lib/
        COMMAND ${CMAKE_STRIP} --strip-unneeded ${MLIBC_IMAGE_LIBS}
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/${PROJECT_NAME} ${CMAKE_BINARY_DIR}/isodir/boot/${PROJECT_NAME}
        COMMENT "Generating your .iso file"
)

set(PROGRAMS_DIR ${CMAKE_SOURCE_DIR}/programs)
set(DASH_BINARY ${PROGRAMS_DIR}/usr/bin/dash)

add_custom_target(programs ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROGRAMS_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROGRAMS_DIR} ${CMAKE_BINARY_DIR}/isodir/
        COMMENT "Copying programs/ into the isodir"
)
add_dependencies(${PROJECT_NAME} programs)

set(BUSYBOX_BINARY ${PROGRAMS_DIR}/usr/bin/busybox)
if(EXISTS ${BUSYBOX_BINARY})
    set(SHELL_BINARY ${BUSYBOX_BINARY})
elseif(EXISTS ${DASH_BINARY})
    set(SHELL_BINARY ${DASH_BINARY})
endif()

if(SHELL_BINARY)
    add_custom_command(TARGET programs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/isodir/bin
            COMMAND ${CMAKE_COMMAND} -E copy ${SHELL_BINARY} ${CMAKE_BINARY_DIR}/isodir/shell
            COMMAND ${CMAKE_COMMAND} -E copy ${SHELL_BINARY} ${CMAKE_BINARY_DIR}/isodir/bin/sh
            COMMENT "Installing ${SHELL_BINARY} as /shell and /bin/sh"
    )
else()
    message(WARNING "no shell found, run tools/install-busybox.sh or tools/install-dash.sh and re-run cmake to get a /shell on the image")
endif()

add_subdirectory(src)
