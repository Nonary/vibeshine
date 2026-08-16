# This is intentionally opt-in: normal Sunshine builds do not contain WDK
# headers or kernel libraries.  The target delegates to the isolated
# MSBuild/WDK project and emits a signed package only when explicitly enabled
# by a Windows packaging lane.
if (WIN32 AND SUNSHINE_BUILD_STEAM_OFFLINE_FILTER_DRIVER)
    set(SUNSHINE_STEAM_OFFLINE_FILTER_PACKAGE_DIR
        "${CMAKE_BINARY_DIR}/steam_offline_filter_package"
        CACHE PATH "Staged Steam Offline Filter driver package directory")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_BUILD_DIR
        "${CMAKE_BINARY_DIR}/steam_offline_filter_build"
        CACHE PATH "Steam Offline Filter WDK build output directory")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_INF2CAT_PATH ""
        CACHE FILEPATH "Optional Inf2Cat.exe path for Steam Offline Filter packaging")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_INFVERIF_PATH ""
        CACHE FILEPATH "Required InfVerif.exe path for Steam Offline Filter packaging")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_SIGNTOOL_PATH ""
        CACHE FILEPATH "Optional signtool.exe path for Steam Offline Filter packaging")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_CERTIFICATE_THUMBPRINT ""
        CACHE STRING "Optional local signing certificate thumbprint for Steam Offline Filter")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_PUBLISHER ""
        CACHE STRING "Required expected publisher substring for Steam Offline Filter signing")
    set(SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_ISSUER ""
        CACHE STRING "Required expected issuer substring for Steam Offline Filter signing")

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|aarch64)$")
        set(_steam_offline_filter_platform ARM64)
    else()
        set(_steam_offline_filter_platform x64)
    endif()

    set(_steam_offline_filter_stamp
        "${SUNSHINE_STEAM_OFFLINE_FILTER_PACKAGE_DIR}/.package.stamp")
    add_custom_command(
        OUTPUT "${_steam_offline_filter_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SUNSHINE_STEAM_OFFLINE_FILTER_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SUNSHINE_STEAM_OFFLINE_FILTER_PACKAGE_DIR}"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/build.ps1"
            -Configuration "$<CONFIG>"
            -Platform "${_steam_offline_filter_platform}"
            -OutputDirectory "${SUNSHINE_STEAM_OFFLINE_FILTER_BUILD_DIR}"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/package.ps1"
            -DriverRoot "${CMAKE_CURRENT_SOURCE_DIR}"
            -BuildOutputDirectory "${SUNSHINE_STEAM_OFFLINE_FILTER_BUILD_DIR}"
            -OutputDirectory "${SUNSHINE_STEAM_OFFLINE_FILTER_PACKAGE_DIR}"
            -Architecture "${_steam_offline_filter_platform}"
            -InfVerifPath "${SUNSHINE_STEAM_OFFLINE_FILTER_INFVERIF_PATH}"
            -Inf2CatPath "${SUNSHINE_STEAM_OFFLINE_FILTER_INF2CAT_PATH}"
            -SignToolPath "${SUNSHINE_STEAM_OFFLINE_FILTER_SIGNTOOL_PATH}"
            -CertificateThumbprint "${SUNSHINE_STEAM_OFFLINE_FILTER_CERTIFICATE_THUMBPRINT}"
            -ExpectedPublisher "${SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_PUBLISHER}"
            -ExpectedIssuer "${SUNSHINE_STEAM_OFFLINE_FILTER_EXPECTED_ISSUER}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_steam_offline_filter_stamp}"
        DEPENDS
            build.ps1
            package.ps1
            SteamOfflineFilter.inf
            SteamOfflineFilter.vcxproj
            steam_offline_filter.c
            steam_offline_filter.h
            steam_offline_filter_ioctl_abi.h
            install.ps1
            uninstall.ps1
        COMMENT "Building and signing the opt-in Vibeshine Steam Offline Filter package"
        VERBATIM)
    add_custom_target(steam_offline_filter_driver_package
        DEPENDS "${_steam_offline_filter_stamp}")
endif()
