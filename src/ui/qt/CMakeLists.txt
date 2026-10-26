find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

qt_add_executable(rufus main.cpp)
target_include_directories(rufus PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rufus PRIVATE Qt6::Widgets rufus_warnings)

install(TARGETS rufus RUNTIME DESTINATION bin)
