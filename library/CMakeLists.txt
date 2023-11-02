file(GLOB_RECURSE LIBRARY_SOURCES ${CMAKE_SOURCE_DIR}/library/*.[c])

# compile all the files and then use ar to make it a static library but don't include the user-start.c file but do compile it
foreach (SOURCE ${LIBRARY_SOURCES})
    get_filename_component(SOURCE_NAME ${SOURCE} NAME_WE)  # Get the file name without extension

    # Compile each source file into an object file
    add_custom_command(
            OUTPUT ${SOURCE_NAME}  # Output object file
            COMMAND ${CMAKE_C_COMPILER} -m32 -Wall -c -ffreestanding -fno-pie -g -std=gnu99 -I ${CMAKE_SOURCE_DIR}/include ${SOURCE} -o ${CMAKE_BINARY_DIR}/library/${SOURCE_NAME}.o
            DEPENDS create-kernel-dir
    )

    if (SOURCE_NAME STREQUAL "user-start")
    else ()
        # Add the object file to the list
        list(APPEND OBJECT_FILES "${SOURCE_NAME}.o")
    endif ()
    list(APPEND LIBRARY_OBJECT_FILES ${SOURCE_NAME})
endforeach()

# compile i686-elf-gcc -m32    -c -o syscall.o syscall.S
add_custom_target(syscall
        COMMAND ${CMAKE_C_COMPILER} -m32 -c -o ${CMAKE_BINARY_DIR}/library/syscall.o syscall.S
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/library
        DEPENDS create-kernel-dir
)
list(APPEND OBJECT_FILES syscall.o)

# just compile user-start.c
#add_library(user-start OBJECT user-start.c)
#target_include_directories(user-start PUBLIC ${CMAKE_SOURCE_DIR}/include)

# make the target available for parent cmake file
add_custom_target(base
        COMMAND ${CMAKE_AR} rv ../libbase.a errno.o syscall.o syscalls.o string.o stdio.o malloc.o kernel_object_string.o nwindow.o
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/library
        DEPENDS ${LIBRARY_OBJECT_FILES} syscall
)
