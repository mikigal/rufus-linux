# The Linux backend: devices (libudev, sysfs), disk access, partitioning, mkfs, the
# format pipeline, and the front end side of rufus-helper
include(GNUInstallDirs)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

add_library(rufus_platform STATIC
	automount.c
	devices.c
	disk.c
	filesystems.c
	format_thread.c
	helper_client.c
	ipc.c
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
# Where the front ends find the helper (unless $RUFUS_HELPER is set)
target_compile_definitions(rufus_platform PRIVATE
	RUFUS_HELPER_INSTALLED="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-helper")
