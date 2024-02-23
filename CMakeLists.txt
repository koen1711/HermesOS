cmake_minimum_required(VERSION 3.25)
project(OS LANGUAGES C ASM ASM_NASM)

set(ARCH "x86_64")

add_subdirectory(src)
#add_subdirectory(library)
#add_subdirectory(user)



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


# first remove iso dir
add_custom_target(OS
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/src/kernel.bin iso/boot/kernel
    # copy the files in build/user to build/iso/bin
    #COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_BINARY_DIR}/user/ ${CMAKE_BINARY_DIR}/iso/bin/
    COMMAND grub-mkrescue -o OS.iso iso
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS src create-kernel-dir
        #user-bin
)
