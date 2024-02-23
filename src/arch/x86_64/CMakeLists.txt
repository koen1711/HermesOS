

# copy the contents of the iso/ dir to the build/iso dir
add_custom_target(grub-config
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory iso
        COMMAND ${CMAKE_COMMAND} -E remove_directory iso
        COMMAND ${CMAKE_COMMAND} -E make_directory iso
        COMMAND ${CMAKE_COMMAND} -E make_directory iso/boot
        COMMAND ${CMAKE_COMMAND} -E make_directory iso/bin
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/iso ${CMAKE_BINARY_DIR}/iso
)


add_library(arch OBJECT boot.asm mb_header.asm long_mode_init.asm)
add_dependencies(arch grub-config)