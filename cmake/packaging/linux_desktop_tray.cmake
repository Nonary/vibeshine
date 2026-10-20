# The machine host has no desktop bus. Start its optional companion only from
# the user's XDG autostart session, never from a privileged service or hook.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR SUNSHINE_BUILD_APPIMAGE OR
        SUNSHINE_BUILD_FLATPAK OR SUNSHINE_BUILD_STEAMOS OR NOT SUNSHINE_TRAY STREQUAL "1")
    return()
endif()

get_filename_component(_tray_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
configure_file("${_tray_source_root}/packaging/linux/vibeshine-tray.desktop.in"
        "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-tray.desktop" @ONLY)
install(PROGRAMS "${_tray_source_root}/packaging/linux/vibeshine-tray.py"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME "vibeshine-tray")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-tray.desktop"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/vibeshine-tray.desktop"
        DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart")
set(VIBESHINE_DESKTOP_TRAY_PACKAGED TRUE)
