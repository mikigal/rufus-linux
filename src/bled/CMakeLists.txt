# Bled (Base Library for Easy Decompression), derived from BusyBox
add_library(bled STATIC
	bled.c bled_size.c crc32.c data_align.c data_extract_all.c data_skip.c decompress_bunzip2.c
	decompress_gunzip.c decompress_uncompress.c decompress_unlzma.c decompress_unxz.c decompress_unzip.c
	decompress_unzstd.c decompress_vtsi.c filter_accept_all.c filter_accept_list.c filter_accept_reject_list.c
	find_list_entry.c fse_decompress.c header_list.c header_skip.c header_verbose_list.c huf_decompress.c
	init_handle.c open_transformer.c seek_by_jump.c seek_by_read.c xz_dec_bcj.c xz_dec_lzma2.c xz_dec_stream.c
	xxhash.c zstd_common.c zstd_decompress.c zstd_decompress_block.c zstd_ddict.c zstd_entropy_common.c
	zstd_error_private.c)
target_include_directories(bled PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(bled PRIVATE BLED_EXPECT_DISK_IMAGE)
target_compile_options(bled PRIVATE -Wno-undef -Wno-strict-aliasing)
target_link_libraries(bled PRIVATE vendor_warnings)
