add_subdirectory(compat)
add_subdirectory(bled)

if(RUFUS_BUILD_GUI)
	add_subdirectory(ui/qt)
endif()
