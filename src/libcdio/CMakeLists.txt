# libcdio (https://www.gnu.org/software/libcdio/), Rufus fork.
# The fork exposes El Torito boot images as virtual "/[BOOT]/" files, which the
# system libcdio does not, so it is built from here rather than linked in.
set(CDIO_PRIVATE_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/driver")

add_library(cdio_driver STATIC
	driver/disc.c driver/ds.c driver/logging.c driver/memory.c driver/read.c driver/sector.c
	driver/track.c driver/util.c driver/_cdio_stdio.c driver/_cdio_stream.c driver/utf8.c
	driver/device_stub.c)
add_library(cdio_iso9660 STATIC iso9660/iso9660.c iso9660/iso9660_fs.c iso9660/rock.c iso9660/xa.c)
add_library(cdio_udf STATIC udf/udf.c udf/udf_file.c udf/udf_fs.c udf/udf_time.c udf/filemode.c)

foreach(lib cdio_driver cdio_iso9660 cdio_udf)
	target_include_directories(${lib} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE ${CDIO_PRIVATE_INCLUDES})
	target_compile_definitions(${lib} PRIVATE HAVE_CONFIG_H
		# Changes the layout of iso9660_stat_t, so consumers must see it too
		PUBLIC DO_NOT_WANT_COMPATIBILITY)
	target_link_libraries(${lib} PRIVATE vendor_warnings)
endforeach()
target_link_libraries(cdio_iso9660 PUBLIC cdio_driver)
target_link_libraries(cdio_udf PUBLIC cdio_driver)

# Convenience target for consumers
add_library(rufus_cdio INTERFACE)
target_link_libraries(rufus_cdio INTERFACE cdio_iso9660 cdio_udf cdio_driver)
