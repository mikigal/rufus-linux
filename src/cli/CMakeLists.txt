# rufus-cli: the command line front end
include(GNUInstallDirs)

add_executable(rufus-cli rufus-cli.c)
target_link_libraries(rufus-cli PRIVATE rufus_platform rufus_warnings)
# Where to find the localization file: the installed one, or the one of the source tree
target_compile_definitions(rufus-cli PRIVATE
	RUFUS_LOC_INSTALLED="${CMAKE_INSTALL_FULL_DATADIR}/rufus/rufus.loc"
	RUFUS_LOC_SOURCE="${PROJECT_SOURCE_DIR}/res/loc/rufus.loc"
	RUFUS_HELPER_INSTALLED="${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-helper")
install(TARGETS rufus-cli)
