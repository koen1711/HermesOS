# compile all the files in user into seperate valid i686 elf executables in /mnt/bin
file(GLOB_RECURSE USER_EXECUTABLES *.c)

foreach(SOURCE ${USER_EXECUTABLES})
    get_filename_component(SOURCE_NAME ${SOURCE} NAME_WE)  # Get the file name without extension

    # Compile each source file into an object file
    add_custom_command(
            OUTPUT ${SOURCE_NAME}  # Output object file
            # also add libbase.a in build/
            # copy all the files with mcopy -i disk.img SOURCE ::bin/USER
            COMMAND ${CMAKE_C_COMPILER} -m32 -Wall -c -ffreestanding -fno-pie -g -std=gnu99 -I ${CMAKE_SOURCE_DIR}/include/ ${SOURCE} -o ${CMAKE_BINARY_DIR}/user/${SOURCE_NAME}.o
            COMMAND ${CMAKE_LINKER} -Ttext 0x80000000 ${CMAKE_BINARY_DIR}/library/user-start.o ${CMAKE_BINARY_DIR}/user/${SOURCE_NAME}.o ${CMAKE_BINARY_DIR}/libbase.a -o ${CMAKE_BINARY_DIR}/user/${SOURCE_NAME}
            COMMAND mcopy -i ${CMAKE_BINARY_DIR}/drive.img ${CMAKE_BINARY_DIR}/user/${SOURCE_NAME} ::${SOURCE_NAME}
            DEPENDS baselib
    )

    # Add the object file to the list
    list(APPEND USER_OBJECT_FILES ${SOURCE_NAME})
endforeach()

add_custom_target(user-bin DEPENDS ${USER_OBJECT_FILES})