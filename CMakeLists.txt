cmake_minimum_required(VERSION 3.25)
project(OS)



set(CMAKE_ASM_FLAGS "-Wall -ffreestanding -fno-pie -g -std=gnu99")
set(CMAKE_C_FLAGS "-Wall -ffreestanding -fno-pie -g -std=gnu99")

enable_language(ASM)

set(CMAKE_C_COMPILER "/home/koen/opt/cross/bin/i686-elf-gcc")
set(CMAKE_CXX_COMPILER "/home/koen/opt/cross/bin/i686-elf-g++")
set(CMAKE_ASM_COMPILER "/home/koen/opt/cross/bin/i686-elf-gcc")
set(CMAKE_AR i686-elf-ar)


file(GLOB_RECURSE LIBRARY_SOURCES library/*.[cS])
file(GLOB_RECURSE USER_SOURCES user/*.c)
file(GLOB_RECURSE KERNEL_SOURCES kernel/*.[cS])
file(GLOB_RECURSE KERNEL_BINARY kernel/*.[cS])

# remove the keymap.us.mac.c file
list(REMOVE_ITEM KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/kernel/hardware/keyboard/keymap/keymap.us.mac.c)
list(REMOVE_ITEM KERNEL_SOURCES ${CMAKE_SOURCE_DIR}/kernel/hardware/keyboard/keymap/keymap.us.pc.c)
list(REMOVE_ITEM KERNEL_BINARY ${CMAKE_SOURCE_DIR}/kernel/hardware/keyboard/keymap/keymap.us.mac.c)
list(REMOVE_ITEM KERNEL_BINARY ${CMAKE_SOURCE_DIR}/kernel/hardware/keyboard/keymap/keymap.us.pc.c)

add_library(base STATIC ${LIBRARY_SOURCES})
target_include_directories(base PRIVATE include)


add_custom_target(create-kernel-dir
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/kernel
    DEPENDS
)

# attempt to get all files in build/kernel
file(GLOB_RECURSE KERNEL_OLD_BUILD_FILES ${CMAKE_BINARY_DIR}/kernel/*)
# rm all files in build/kernel
if (KERNEL_OLD_BUILD_FILES)
    add_custom_target(clean-kernel-dir
        COMMAND ${CMAKE_COMMAND} -E remove ${KERNEL_OLD_BUILD_FILES}
        DEPENDS create-kernel-dir
    )
else ()
    add_custom_target(clean-kernel-dir
        DEPENDS create-kernel-dir
    )
endif ()

foreach(SOURCE ${KERNEL_SOURCES})
    get_filename_component(SOURCE_NAME ${SOURCE} NAME_WE)  # Get the file name without extension

    # Compile each source file into an object file
    add_custom_command(
            OUTPUT ${CMAKE_BINARY_DIR}/${SOURCE_NAME}.o  # Output object file
            COMMAND ${CMAKE_C_COMPILER} -m32 -Wall -c -ffreestanding -fno-pie -g -std=gnu99 -I ${CMAKE_SOURCE_DIR}/include ${SOURCE} -o ${CMAKE_BINARY_DIR}/kernel/${SOURCE_NAME}.o
            DEPENDS clean-kernel-dir
    )

    # Add the object file to the list
    list(APPEND OBJECT_FILES ${CMAKE_BINARY_DIR}/${SOURCE_NAME}.o)
endforeach()

add_custom_target(kernel DEPENDS ${OBJECT_FILES})
#add_executable(kernel ${KERNEL_SOURCES})
#add_dependencies(kernel clean-kernel-dir )
#target_include_directories(kernel PRIVATE include)


# the list of objects needed to copy is just KERNEL_SOURCES but replace ${CMAKE_SOURCE_DIR} with ${CMAKE_BINARY_DIR}/CMakeFiles/kernel.dir
list(TRANSFORM KERNEL_BINARY REPLACE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/CMakeFiles/kernel.dir OUTPUT_VARIABLE KERNEL_OBJECTS)
# rename all the .c files to .c.o and .S files to .S.o
set(TRANSFORMED_KERNEL_OBJECTS "")

foreach(INPUT_FILE ${KERNEL_OBJECTS})
    string(REGEX REPLACE "\\.c$" ".o" TRANSFORMED_FILE "${INPUT_FILE}")
    string(REGEX REPLACE "\\.S$" ".o" TRANSFORMED_FILE "${TRANSFORMED_FILE}")
    list(APPEND TRANSFORMED_KERNEL_OBJECTS "${TRANSFORMED_FILE}")
endforeach()

set(KERNEL_OBJECTS "")


# remove the path before the file name and then set it to ${CMAKE_BINARY_DIR}/kernel
foreach(INPUT_FILE ${TRANSFORMED_KERNEL_OBJECTS})
    string(REGEX REPLACE "^.*/" "${CMAKE_BINARY_DIR}/kernel/" TRANSFORMED_FILE "${INPUT_FILE}")
    list(APPEND KERNEL_OBJECTS "${TRANSFORMED_FILE}")
endforeach()
list(REMOVE_ITEM KERNEL_OBJECTS ${CMAKE_BINARY_DIR}/kernel/bootblock.o)
list(REMOVE_ITEM KERNEL_OBJECTS ${CMAKE_BINARY_DIR}/kernel/event.o)

add_custom_target(bootblock
        COMMAND i686-elf-ld  -Ttext 0 bootblock.o -o bootblock.elf
        COMMAND i686-elf-objcopy -O binary bootblock.elf bootblock
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/kernel
        DEPENDS kernel
)

# move kernelcore.o to front of kernel objects and after that main.o
list(REMOVE_ITEM KERNEL_OBJECTS ${CMAKE_BINARY_DIR}/kernel/kernelcore.o)
list(REMOVE_ITEM KERNEL_OBJECTS ${CMAKE_BINARY_DIR}/kernel/main.o)
# add the items to the front of the list
list(INSERT KERNEL_OBJECTS 0 ${CMAKE_BINARY_DIR}/kernel/kernelcore.o)
list(INSERT KERNEL_OBJECTS 1 ${CMAKE_BINARY_DIR}/kernel/main.o)

add_custom_target(kernel-elf
    COMMAND i686-elf-ld -Ttext 0x10000 ${KERNEL_OBJECTS} -o kernel.elf
    COMMAND i686-elf-objcopy -O binary kernel.elf kernel
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/kernel
    DEPENDS bootblock
)

add_custom_target(kernel-img
    COMMAND cat bootblock kernel /dev/zero | head -c 1474560 > basekernel.img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/kernel
    DEPENDS kernel-elf
)

# first remove iso dir
add_custom_target(OS
    COMMAND ${CMAKE_COMMAND} -E remove_directory iso
    COMMAND ${CMAKE_COMMAND} -E make_directory iso
    COMMAND ${CMAKE_COMMAND} -E make_directory iso/boot
    COMMAND ${CMAKE_COMMAND} -E make_directory iso/bin
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/kernel/basekernel.img iso/boot/
    COMMAND genisoimage -input-charset utf-8 -iso-level 2 -J -R -o OS.iso -b boot/basekernel.img iso
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS kernel-img)
