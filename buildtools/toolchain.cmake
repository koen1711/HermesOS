list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

# We use "Generic" so cmake can expect to build for `none` platform.
set(CMAKE_SYSTEM_NAME Generic)
# Our buildroot toolchain is for `x86_64`
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Toolchain Prefix
set(TC_PREFIX x86_64-elf-)
set(TC_PATH ${CMAKE_CURRENT_SOURCE_DIR}/buildtools/cross/bin/)

set(TC_EXT "" )

set(CMAKE_ASM_LINK_EXECUTABLE "ld <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
set(CMAKE_ASM_OBJECT_FORMAT elf64)

# Shun all C before C99
set(CMAKE_C_FLAGS "-ffreestanding -g -nostdlib" CACHE INTERNAL "C Compiler options")
set(CMAKE_ASM_FLAGS "" CACHE INTERNAL "ASM Compiler options")
set(CMAKE_EXE_LINKER_FLAGS "" CACHE INTERNAL "Linker options")

# Debug flags
set(CMAKE_ASM_FLAGS_DEBUG "" CACHE INTERNAL "ASM Compiler options for debug build type")
set(CMAKE_C_FLAGS_DEBUG "" CACHE INTERNAL "C Compiler options for debug build type")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "" CACHE INTERNAL "Linker options for debug build type")

# Release flags
set(CMAKE_ASM_FLAGS_RELEASE "") # ASM Compiler options for release build type
set(CMAKE_C_FLAGS_RELEASE "") # C Compiler options for release build type
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "") # Linker options for release build type

# Binaries
set(CMAKE_LINKER ${TC_PATH}${TC_PREFIX}ld${TC_EXT} CACHE INTERNAL "Linker Binary")
set(CMAKE_ASM_COMPILER ${TC_PATH}${TC_PREFIX}as${TC_EXT} CACHE INTERNAL "ASM Compiler")
set(CMAKE_C_COMPILER ${TC_PATH}${TC_PREFIX}gcc${TC_EXT} CACHE INTERNAL "C Compiler")
set(CMAKE_OBJCOPY ${TC_PATH}${TC_PREFIX}objcopy${TC_EXT} CACHE INTERNAL "Objcopy Binary")
set(CMAKE_OBJDUMP ${TC_PATH}${TC_PREFIX}objdump${TC_EXT} CACHE INTERNAL "Objdump Binary")

