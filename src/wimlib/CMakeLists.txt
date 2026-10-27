# wimlib 1.14.5 (https://wimlib.net), with Rufus patches: see README.rufus.md
file(GLOB WIMLIB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
# Without the NTFS-3G mode, which only Windows To Go would use (not ported)
list(FILTER WIMLIB_SOURCES EXCLUDE REGEX "/ntfs-3g_[a-z]+\\.c$")

add_library(wim STATIC ${WIMLIB_SOURCES})
target_include_directories(wim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
	PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(wim PRIVATE HAVE_CONFIG_H BUILDING_WIMLIB _RUFUS)
target_compile_options(wim PRIVATE -fvisibility=hidden -fno-common)
target_link_libraries(wim PRIVATE vendor_warnings cdio_iso9660 cdio_udf)
find_package(Threads REQUIRED)
target_link_libraries(wim PUBLIC Threads::Threads)
