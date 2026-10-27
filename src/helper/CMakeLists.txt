# rufus-helper: the privileged part, started through pkexec (polkit action ie.rufus.helper)
include(GNUInstallDirs)

add_executable(rufus-helper rufus-helper.c)
target_link_libraries(rufus-helper PRIVATE rufus_platform rufus_warnings)
# It only reads the localization file of the installation, or of the source tree it was built from
target_compile_definitions(rufus-helper PRIVATE
	RUFUS_LOC_INSTALLED="${CMAKE_INSTALL_FULL_DATADIR}/rufus/rufus.loc"
	RUFUS_LOC_SOURCE="${PROJECT_SOURCE_DIR}/res/loc/rufus.loc")
install(TARGETS rufus-helper DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}")

# The polkit action, for the installed helper
set(RUFUS_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/rufus-helper")
configure_file("${PROJECT_SOURCE_DIR}/res/linux/ie.rufus.helper.policy.in"
	"${CMAKE_CURRENT_BINARY_DIR}/ie.rufus.helper.policy" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ie.rufus.helper.policy"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/polkit-1/actions")
