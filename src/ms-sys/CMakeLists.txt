# ms-sys: boot record writers (https://ms-sys.sourceforge.io/)
add_library(mssys STATIC fat12.c fat16.c fat32.c ntfs.c partition_info.c br.c file.c)
target_include_directories(mssys PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/inc")
target_link_libraries(mssys PRIVATE vendor_warnings)
