

add_subdirectory(arch)
add_subdirectory(kernel)

# make a ARCH_OBJECTS list based off of $<TARGET_OBJECTS:arch>

file(GLOB_RECURSE ARCH_OBJECTS ${CMAKE_SOURCE_DIR}/src/arch/x86_64/*.asm)
file(GLOB_RECURSE KERNEL_OBJECTS ${CMAKE_SOURCE_DIR}/src/kernel/*.c)

# remove mb_header.asm from the list and insert it back at the second position
list(REMOVE_ITEM ARCH_OBJECTS ${CMAKE_SOURCE_DIR}/src/arch/x86_64/mb_header.asm)
list(INSERT ARCH_OBJECTS 1 ${CMAKE_SOURCE_DIR}/src/arch/x86_64/mb_header.asm)

# remove start.c from the list and insert it back at the first position
list(REMOVE_ITEM KERNEL_OBJECTS ${CMAKE_SOURCE_DIR}/src/kernel/start.c)
list(INSERT KERNEL_OBJECTS 0 ${CMAKE_SOURCE_DIR}/src/kernel/start.c)

# now replace those with the place they are in the build directory and the suffix and CMakeFiles before the filename
string(REPLACE ${CMAKE_SOURCE_DIR}/src/arch/x86_64/ ${CMAKE_BINARY_DIR}/src/arch/x86_64/CMakeFiles/arch.dir/ ARCH_OBJECTS "${ARCH_OBJECTS}")
string(REPLACE ${CMAKE_SOURCE_DIR}/src/kernel/ ${CMAKE_BINARY_DIR}/src/kernel/CMakeFiles/kernel.dir/ KERNEL_OBJECTS "${KERNEL_OBJECTS}")
string(REPLACE .asm .asm.obj ARCH_OBJECTS "${ARCH_OBJECTS}")
string(REPLACE .c .c.obj KERNEL_OBJECTS "${KERNEL_OBJECTS}")




        # link the boot object library with the kernel object library
add_custom_target(src DEPENDS arch kernel
        COMMAND ld -n -o ${CMAKE_BINARY_DIR}/src/kernel.bin -T ${CMAKE_SOURCE_DIR}/src/arch/x86_64/linker.ld ${ARCH_OBJECTS} ${KERNEL_OBJECTS}
)