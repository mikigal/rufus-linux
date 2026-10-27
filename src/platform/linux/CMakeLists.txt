# The Linux backend: devices (libudev, sysfs), disk access and, later, partitioning and mkfs
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

add_library(rufus_platform STATIC
	automount.c
	devices.c
	disk.c
	filesystems.c
	format_thread.c
	mounts.c
	processes.c
	../../format_ext.c
	../../format_fat32.c
	../../syslinux.c
	../../badblocks.c)
target_include_directories(rufus_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rufus_platform
	PUBLIC rufus_core syslinux_installer PkgConfig::UDEV PkgConfig::EXT2FS
	PRIVATE rufus_warnings)
