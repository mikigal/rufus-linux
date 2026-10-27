cmake_minimum_required(VERSION 3.25)

project(rufus
	VERSION 4.16.0
	DESCRIPTION "The Reliable USB Formatting Utility (Linux port)"
	HOMEPAGE_URL "https://github.com/mikigal/rufus-linux"
	LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "This port of Rufus only targets Linux")
endif()

option(RUFUS_BUILD_GUI "Build the Qt 6 user interface" ON)
option(RUFUS_BUILD_TESTS "Build the unit tests" ON)
option(RUFUS_WERROR "Treat warnings in Rufus' own code as errors" OFF)
option(RUFUS_E2E_TESTS "Register the end-to-end tests (need QEMU, see tests/e2e/README.md)" OFF)
option(RUFUS_GUI_TESTS "Register the GUI tests (need KWin or sway and AT-SPI, see tests/gui/README.md)" OFF)

# Rufus has always defaulted to debug builds
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_definitions(_GNU_SOURCE _FILE_OFFSET_BITS=64)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RufusWarnings)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
# System libraries (vendored ones live under src/)
pkg_check_modules(EXT2FS REQUIRED IMPORTED_TARGET GLOBAL ext2fs com_err)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET GLOBAL libcurl>=7.85)
pkg_check_modules(HIVEX IMPORTED_TARGET GLOBAL hivex)
if(NOT HIVEX_FOUND)
	message(WARNING "hivex was not found: the registry keys of the Windows User Experience will be created "
		"through unattend.xml instead")
endif()

add_subdirectory(src)

if(RUFUS_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
