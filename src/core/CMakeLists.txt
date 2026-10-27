# librufus-core: the platform-independent logic, shared by the GUI, the
# privileged helper and the CLI. It must never include a Win32 or UI header.
# Upstream files keep their place in src/, to ease merges; new ones live here.
add_library(rufus_core STATIC
	globals.c
	progress.c
	rules.c
	ui_iface.c
	../cregex_compile.c
	../cregex_parse.c
	../cregex_vm.c
	../localization.c
	../parser.c
	../stdfn.c
	../stdio.c
	../xml.c)
target_include_directories(rufus_core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/src")
target_link_libraries(rufus_core
	PUBLIC rufus_compat bled m
	PRIVATE rufus_warnings)
