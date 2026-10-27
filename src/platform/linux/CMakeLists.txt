# The Linux backend: devices (libudev, sysfs) and, later, disk access, partitioning and mkfs
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

add_library(rufus_platform STATIC
	devices.c
	mounts.c)
target_include_directories(rufus_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rufus_platform
	PUBLIC rufus_core PkgConfig::UDEV
	PRIVATE rufus_warnings)
