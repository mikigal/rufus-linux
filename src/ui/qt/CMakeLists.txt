# rufus: the Qt 6 user interface
include(GNUInstallDirs)
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets DBus)
qt_standard_project_setup()

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h" @ONLY)

qt_add_executable(rufus
	app.cpp
	device_monitor.cpp
	dialogs.cpp
	log_window.cpp
	main.cpp
	main_window.cpp
	progress_bar.cpp
	taskbar.cpp
	test_hooks.cpp
	ui_bridge.cpp)
target_include_directories(rufus PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(rufus PRIVATE rufus_platform Qt6::Widgets Qt6::DBus rufus_warnings)
# QT_NO_KEYWORDS keeps signals/slots/emit out of the C headers, which need C++23 for _Atomic
target_compile_definitions(rufus PRIVATE QT_NO_KEYWORDS
	RUFUS_LOC_INSTALLED="${CMAKE_INSTALL_FULL_DATADIR}/rufus/rufus.loc"
	RUFUS_LOC_SOURCE="${PROJECT_SOURCE_DIR}/res/loc/rufus.loc")
target_compile_features(rufus PRIVATE cxx_std_23)
# No .ui files (and ui_bridge.h is not one)
set_target_properties(rufus PROPERTIES AUTOUIC OFF)

# The toolbar icons
file(GLOB icon_files RELATIVE "${PROJECT_SOURCE_DIR}/res/icons" "${PROJECT_SOURCE_DIR}/res/icons/*.png")
list(TRANSFORM icon_files PREPEND "${PROJECT_SOURCE_DIR}/res/icons/")
qt_add_resources(rufus "icons" PREFIX "/icons" BASE "${PROJECT_SOURCE_DIR}/res/icons" FILES ${icon_files})

install(TARGETS rufus RUNTIME DESTINATION bin)
# The desktop file, whose name is the Wayland app_id (QGuiApplication::desktopFileName()), and the icons
install(FILES "${PROJECT_SOURCE_DIR}/res/linux/rufus.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
foreach(size 16 24 32 48 64 128 256 512)
	install(FILES "${PROJECT_SOURCE_DIR}/res/icons/rufus-${size}.png"
		DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps" RENAME rufus.png)
endforeach()
