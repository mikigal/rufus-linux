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

rufus_add_test(test_wimlib test_wimlib.c)
target_link_libraries(test_wimlib PRIVATE wim)

rufus_add_test(test_libcdio test_libcdio.c)
target_link_libraries(test_libcdio PRIVATE rufus_cdio)

rufus_add_test(test_ext2fs test_ext2fs.c)
target_link_libraries(test_ext2fs PRIVATE PkgConfig::EXT2FS)

rufus_add_test(test_core test_core.c)
target_link_libraries(test_core PRIVATE rufus_core)

rufus_add_test(test_hash test_hash.c)
target_link_libraries(test_hash PRIVATE rufus_core)

rufus_add_test(test_iso test_iso.c)
target_link_libraries(test_iso PRIVATE rufus_core)

rufus_add_test(test_wue test_wue.c)
target_link_libraries(test_wue PRIVATE rufus_core)

rufus_add_test(test_scan test_scan.c)
target_link_libraries(test_scan PRIVATE rufus_core)

rufus_add_test(test_bootcheck test_bootcheck.c)
target_link_libraries(test_bootcheck PRIVATE rufus_core)

rufus_add_test(test_threads test_threads.c)
target_link_libraries(test_threads PRIVATE rufus_core)

rufus_add_test(test_devices test_devices.c)
target_link_libraries(test_devices PRIVATE rufus_platform)

rufus_add_test(test_partition test_partition.c)
target_link_libraries(test_partition PRIVATE rufus_core)

rufus_add_test(test_dos test_dos.c)
target_link_libraries(test_dos PRIVATE rufus_core)

rufus_add_test(test_write test_write.c)
target_link_libraries(test_write PRIVATE rufus_platform)

rufus_add_test(test_filesystems test_filesystems.c)
target_link_libraries(test_filesystems PRIVATE rufus_platform)

rufus_add_test(test_badblocks test_badblocks.c)
target_link_libraries(test_badblocks PRIVATE rufus_platform)

rufus_add_test(test_ipc test_ipc.c)
target_link_libraries(test_ipc PRIVATE rufus_platform)
set_property(TEST test_ipc APPEND PROPERTY ENVIRONMENT "RUFUS_TEST_HELPER=$<TARGET_FILE:rufus-helper>")

rufus_add_test(test_disk test_disk.c)
target_link_libraries(test_disk PRIVATE rufus_platform)

# Erases a USB stick: only run as root in the VM of tests/e2e/test_disk.py (skipped otherwise)
rufus_add_test(test_disk_vm test_disk_vm.c)
target_link_libraries(test_disk_vm PRIVATE rufus_platform)
rufus_add_test(test_format_vm test_format_vm.c)
target_link_libraries(test_format_vm PRIVATE rufus_platform)
rufus_add_test(test_helper_vm test_helper_vm.c)
target_link_libraries(test_helper_vm PRIVATE rufus_platform)

# A fake rufus-helper, for the tests of the front ends (RUFUS_FAKE_HELPER)
add_executable(rufus-fake-helper fake_helper.c)
target_link_libraries(rufus-fake-helper PRIVATE rufus_platform rufus_warnings)

# The rules are checked against the original Windows functions (rules_oracle/rules_win.c)
rufus_add_test(test_rules test_rules.c rules_oracle/rules_win.c)
target_link_libraries(test_rules PRIVATE rufus_core)
# The oracle is verbatim upstream code, written for LLP64 format specifiers
set_source_files_properties(rules_oracle/rules_win.c PROPERTIES COMPILE_OPTIONS "-Wno-format-security;-Wno-format;-Wno-unused-function;-Wno-unused-variable")

# End-to-end tests: ctest -L e2e
if(RUFUS_E2E_TESTS)
	find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
	add_test(NAME e2e_harness COMMAND Python3::Interpreter -m unittest -v test_harness
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_harness PROPERTIES LABELS e2e TIMEOUT 3600
		ENVIRONMENT "RUFUS_BUILD_DIR=${PROJECT_BINARY_DIR}")
	add_test(NAME e2e_devices COMMAND Python3::Interpreter -m unittest -v test_devices
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_devices PROPERTIES LABELS e2e TIMEOUT 1800
		ENVIRONMENT "RUFUS_CLI=$<TARGET_FILE:rufus-cli>")
	add_test(NAME e2e_disk COMMAND Python3::Interpreter -m unittest -v test_disk
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_disk PROPERTIES LABELS e2e TIMEOUT 1800
		ENVIRONMENT "RUFUS_TEST_DISK_VM=$<TARGET_FILE:test_disk_vm>;RUFUS_TEST_BADBLOCKS=$<TARGET_FILE:test_badblocks>")
	add_test(NAME e2e_format COMMAND Python3::Interpreter -m unittest -v test_format
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_format PROPERTIES LABELS e2e TIMEOUT 3600
		ENVIRONMENT "RUFUS_TEST_FORMAT_VM=$<TARGET_FILE:test_format_vm>")
	add_test(NAME e2e_cli COMMAND Python3::Interpreter -m unittest -v test_cli
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_cli PROPERTIES LABELS e2e TIMEOUT 3600
		ENVIRONMENT "RUFUS_CLI=$<TARGET_FILE:rufus-cli>;RUFUS_HELPER=$<TARGET_FILE:rufus-helper>;RUFUS_TEST_HELPER_VM=$<TARGET_FILE:test_helper_vm>")
	add_test(NAME e2e_media COMMAND Python3::Interpreter -m unittest -v test_media
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/e2e")
	set_tests_properties(e2e_media PROPERTIES LABELS e2e TIMEOUT 5400
		ENVIRONMENT "RUFUS_CLI=$<TARGET_FILE:rufus-cli>;RUFUS_HELPER=$<TARGET_FILE:rufus-helper>")
endif()

# GUI tests: ctest -L gui
if(RUFUS_GUI_TESTS AND RUFUS_BUILD_GUI)
	find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
	foreach(compositor KWin Sway)
		string(TOLOWER ${compositor} name)
		add_test(NAME gui_${name} COMMAND Python3::Interpreter -m unittest -v test_gui.${compositor}
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gui")
		set_tests_properties(gui_${name} PROPERTIES LABELS gui TIMEOUT 1800
			ENVIRONMENT "RUFUS_GUI=$<TARGET_FILE:rufus>;RUFUS_FAKE_HELPER=$<TARGET_FILE:rufus-fake-helper>")
	endforeach()
endif()
