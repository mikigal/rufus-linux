include(GNUInstallDirs)

# The localization file, relative to the bin and libexec directories (GetInstalledFile())
file(RELATIVE_PATH RUFUS_LOC_FROM_BINDIR "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/rufus/rufus.loc")
file(RELATIVE_PATH RUFUS_LOC_FROM_LIBEXECDIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/rufus/rufus.loc")

add_subdirectory(compat)
add_subdirectory(bled)
add_subdirectory(ms-sys)
add_subdirectory(syslinux)
add_subdirectory(libcdio)
add_subdirectory(wimlib)
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(cli)
add_subdirectory(helper)

if(RUFUS_BUILD_GUI)
	add_subdirectory(ui/qt)
endif()

# The messages of every front end (RUFUS_LOC_INSTALLED)
install(FILES "${PROJECT_SOURCE_DIR}/res/loc/rufus.loc" DESTINATION "${CMAKE_INSTALL_DATADIR}/rufus")
