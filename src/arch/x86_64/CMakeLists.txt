
file(GLOB_RECURSE ASM_SOURCES "*.asm")

target_sources(${PROJECT_NAME} PUBLIC ${ASM_SOURCES})
set_source_files_properties(${ASM_SOURCES} PROPERTIES LANGUAGE ASM)