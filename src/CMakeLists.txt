add_subdirectory(compat)
add_subdirectory(bled)
add_subdirectory(ms-sys)
add_subdirectory(syslinux)
add_subdirectory(libcdio)

if(RUFUS_BUILD_GUI)
	add_subdirectory(ui/qt)
endif()
