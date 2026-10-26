# Syslinux installer libraries (https://www.syslinux.org)
add_library(syslinux_fat STATIC libfat/cache.c libfat/fatchain.c libfat/open.c libfat/searchdir.c libfat/dumpdir.c)
target_include_directories(syslinux_fat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libfat")
target_link_libraries(syslinux_fat PRIVATE vendor_warnings)

add_library(syslinux_installer STATIC libinstaller/fs.c libinstaller/setadv.c libinstaller/syslxmod.c)
target_include_directories(syslinux_installer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libinstaller")
target_compile_options(syslinux_installer PRIVATE -Wno-address-of-packed-member)
target_link_libraries(syslinux_installer PRIVATE vendor_warnings)
