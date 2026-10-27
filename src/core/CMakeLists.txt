# librufus-core: the platform-independent logic, shared by the GUI, the
# privileged helper and the CLI. It must never include a Win32 or UI header.
# Upstream files keep their place in src/, to ease merges; new ones live here.
add_library(rufus_core STATIC
	analyze.c
	autorun.c
	boot_check.c
	boot_records.c
	devices.c
	download.c
	globals.c
	hdd.c
	image_scan.c
	partition.c
	options.c
	progress.c
	resource.c
	rules.c
	write_drive.c
	ui_iface.c
	../cregex_compile.c
	../cregex_parse.c
	../cregex_vm.c
	../dos.c
	../dos_locale.c
	../hash.c
	../iso.c
	../localization.c
	../parser.c
	../pki.c
	../stdfn.c
	../stdio.c
	../vhd.c
	../wue.c
	../xml.c)
target_compile_definitions(rufus_core PRIVATE
	RUFUS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	RUFUS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	RUFUS_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_include_directories(rufus_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rufus_core
	PUBLIC rufus_compat bled rufus_cdio syslinux_fat wim mssys m Threads::Threads OpenSSL::Crypto
	PRIVATE rufus_warnings)

# The payloads that rufus.rc embeds (see resource.c)
file(STRINGS resources.h resource_lines REGEX "^RESOURCE\\(")
set(resource_files "")
foreach(line IN LISTS resource_lines)
	string(REGEX REPLACE "^RESOURCE\\([A-Z0-9_]+, \"([^\"]+)\"\\)$" "\\1" file "${line}")
	list(APPEND resource_files "${PROJECT_SOURCE_DIR}/res/${file}")
endforeach()
set_source_files_properties(resource.c PROPERTIES
	COMPILE_DEFINITIONS "RUFUS_RES_DIR=\"${PROJECT_SOURCE_DIR}/res\""
	OBJECT_DEPENDS "${resource_files};${CMAKE_CURRENT_SOURCE_DIR}/resources.h")
