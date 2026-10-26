# Header-only Windows type/CRT compatibility layer (see compat.h)
add_library(rufus_compat INTERFACE)
target_include_directories(rufus_compat INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
