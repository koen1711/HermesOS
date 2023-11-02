cmake_minimum_required(VERSION 3.25)
project(OS)



set(CMAKE_ASM_FLAGS "-Wall -ffreestanding -fno-pie -g -std=gnu99")
set(CMAKE_C_FLAGS "-Wall -ffreestanding -fno-pie -g -std=gnu99")

enable_language(ASM)

set(CMAKE_C_COMPILER ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-gcc)
set(CMAKE_CXX_COMPILER ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-g++)
set(CMAKE_ASM_COMPILER ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-gcc)
set(CMAKE_AR ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-ar)
set(CMAKE_OBJCOPY ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-objcopy)
set(CMAKE_C_LINKER ${CMAKE_SOURCE_DIR}/buildtools/cross/bin/i686-elf-ld)

add_subdirectory(kernel)
add_subdirectory(library)
add_subdirectory(user)



file(GLOB_RECURSE DRIVE ${CMAKE_BINARY_DIR}/drive.img)
if (DRIVE)
    add_custom_target(create-kernel-dir)
else ()
    add_custom_target(create-kernel-dir
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kernel
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/library
        COMMAND dd if=/dev/zero of=${CMAKE_BINARY_DIR}/drive.img bs=512 count=2880
        COMMAND mkfs.fat -F 12 ${CMAKE_BINARY_DIR}/drive.img
        COMMAND mmd -i ${CMAKE_BINARY_DIR}/drive.img ::bin
        COMMAND mmd -i ${CMAKE_BINARY_DIR}/drive.img ::bin/kernel
    )
endif ()



#add_executable(kernel ${KERNEL_SOURCES})
#add_dependencies(kernel clean-kernel-dir )
#target_include_directories(kernel PRIVATE include)






# first remove iso dir
add_custom_target(OS
    COMMAND ${CMAKE_COMMAND} -E remove_directory iso
    COMMAND ${CMAKE_COMMAND} -E make_directory iso
    COMMAND ${CMAKE_COMMAND} -E make_directory iso/boot
    COMMAND ${CMAKE_COMMAND} -E make_directory iso/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/kernel/basekernel.img iso/boot/
    # copy the files in build/user to build/iso/bin
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/user/ ${CMAKE_BINARY_DIR}/iso/bin/
    COMMAND genisoimage -input-charset utf-8 -iso-level 2 -J -R -o OS.iso -b boot/basekernel.img iso
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS kernel-img user-bin)
