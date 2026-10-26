# Unit tests. Each test is a standalone executable returning 0 on success.
function(rufus_add_test name)
	add_executable(${name} ${ARGN})
	target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
	target_link_libraries(${name} PRIVATE rufus_warnings)
	add_test(NAME ${name} COMMAND ${name})
	# Exit code 77 means that a prerequisite (e.g. an external tool) is missing
	set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77
		ENVIRONMENT "RUFUS_TEST_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data;RUFUS_SOURCE_DIR=${PROJECT_SOURCE_DIR}")
endfunction()

rufus_add_test(test_compat test_compat.c)
target_link_libraries(test_compat PRIVATE rufus_compat)

rufus_add_test(test_bled test_bled.c)
target_link_libraries(test_bled PRIVATE bled)

rufus_add_test(test_mssys test_mssys.c)
target_link_libraries(test_mssys PRIVATE mssys)

rufus_add_test(test_syslinux test_syslinux.c)
target_link_libraries(test_syslinux PRIVATE syslinux_fat syslinux_installer)

rufus_add_test(test_libcdio test_libcdio.c)
target_link_libraries(test_libcdio PRIVATE rufus_cdio)
