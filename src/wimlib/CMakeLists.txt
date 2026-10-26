# wimlib 1.14.5 (https://wimlib.net), with Rufus patches: see README.rufus.md
file(GLOB WIMLIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(FILTER WIMLIB_SOURCES EXCLUDE REGEX "/ntfs-3g_[a-z]+\\.c$")

pkg_check_modules(LIBNTFS3G IMPORTED_TARGET libntfs-3g>=2011.4.12)
if(LIBNTFS3G_FOUND)
	list(APPEND WIMLIB_SOURCES src/ntfs-3g_apply.c src/ntfs-3g_capture.c)
endif()

add_library(wim STATIC ${WIMLIB_SOURCES})
target_include_directories(wim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(wim PRIVATE HAVE_CONFIG_H BUILDING_WIMLIB _RUFUS)
target_compile_options(wim PRIVATE -fvisibility=hidden -fno-common)
target_link_libraries(wim PRIVATE vendor_warnings cdio_iso9660 cdio_udf)
if(LIBNTFS3G_FOUND)
	target_compile_definitions(wim PRIVATE WITH_NTFS_3G)
	target_link_libraries(wim PRIVATE PkgConfig::LIBNTFS3G)
endif()
find_package(Threads REQUIRED)
target_link_libraries(wim PUBLIC Threads::Threads)
