file(GLOB_RECURSE LIBRARY_SOURCES ${CMAKE_SOURCE_DIR}/library/*.[c])

add_library(baselib STATIC ${LIBRARY_SOURCES})
target_include_directories(baselib PUBLIC ${CMAKE_SOURCE_DIR}/include)

