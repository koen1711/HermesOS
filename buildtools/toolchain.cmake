# Toolchain file for cross-compiling the kernel to x86_64-elf.
# Usage:  cmake -B build -DCMAKE_TOOLCHAIN_FILE=buildtools/toolchain.cmake

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Bare-metal: no crt0/libc, so CMake's "can the compiler link an exe?" probe
# would fail. Have it compile a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TC_PREFIX x86_64-elf-)
set(TC_PATH ${CMAKE_CURRENT_LIST_DIR}/cross/bin/)

if(CMAKE_HOST_WIN32)
    set(TC_EXT ".exe")
else()
    set(TC_EXT "")
endif()

# Build the cross-compiler in-tree if it's not already present.
# First run takes 30-60 minutes; subsequent runs short-circuit on the existence check.
if(NOT EXISTS ${TC_PATH}${TC_PREFIX}gcc${TC_EXT})
    if(CMAKE_HOST_WIN32)
        set(_tc_install_cmd cmd /c ${CMAKE_CURRENT_LIST_DIR}/install-buildtools-windows.bat)
        set(_tc_script_name install-buildtools-windows.bat)
    elseif(CMAKE_HOST_UNIX)
        set(_tc_install_cmd sh ${CMAKE_CURRENT_LIST_DIR}/install-buildtools-linux.sh)
        set(_tc_script_name install-buildtools-linux.sh)
    else()
        message(FATAL_ERROR
            "Cross-compiler not found at ${TC_PATH}${TC_PREFIX}gcc${TC_EXT}.\n"
            "No auto-build script exists for this host. Build or install an "
            "x86_64-elf-gcc toolchain into buildtools/cross/ manually.")
    endif()

    message(STATUS "Cross-compiler not found at ${TC_PATH} — building via ${_tc_script_name} (this takes a while)")
    execute_process(
        COMMAND ${_tc_install_cmd}
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE _tc_build_result)
    if(NOT _tc_build_result EQUAL 0)
        message(FATAL_ERROR "${_tc_script_name} failed (exit ${_tc_build_result})")
    endif()
    if(NOT EXISTS ${TC_PATH}${TC_PREFIX}gcc${TC_EXT})
        message(FATAL_ERROR "Install script completed but ${TC_PATH}${TC_PREFIX}gcc${TC_EXT} is still missing")
    endif()
endif()

set(CMAKE_ASM_OBJECT_FORMAT elf64)

set(CMAKE_C_COMPILER   ${TC_PATH}${TC_PREFIX}gcc${TC_EXT})
set(CMAKE_ASM_COMPILER ${TC_PATH}${TC_PREFIX}as${TC_EXT})
set(CMAKE_LINKER       ${TC_PATH}${TC_PREFIX}ld${TC_EXT})
set(CMAKE_OBJCOPY      ${TC_PATH}${TC_PREFIX}objcopy${TC_EXT})
set(CMAKE_OBJDUMP      ${TC_PATH}${TC_PREFIX}objdump${TC_EXT})

# The project's CMakeLists.txt drives compile/link flags via target_*_options.
# Empty CMake's default per-config flags so they don't double up or fight ours.
set(CMAKE_C_FLAGS_DEBUG       "-g" CACHE INTERNAL "")
set(CMAKE_C_FLAGS_RELEASE     ""   CACHE INTERNAL "")
set(CMAKE_ASM_FLAGS_DEBUG     ""   CACHE INTERNAL "")
set(CMAKE_ASM_FLAGS_RELEASE   ""   CACHE INTERNAL "")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG   "" CACHE INTERNAL "")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "" CACHE INTERNAL "")
