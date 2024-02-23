
# get all files in the directory and insert start.c from the arch dir at the beginning
file(GLOB_RECURSE SOURCES "*.c")

# add the executable
add_library(kernel STATIC OBJECT ${SOURCES})
target_include_directories(kernel PUBLIC ../../include)
