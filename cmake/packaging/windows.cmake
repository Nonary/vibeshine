# windows specific packaging
include("${CMAKE_SOURCE_DIR}/cmake/packaging/windows_virtual_display_contract.cmake")

install(TARGETS sunshine RUNTIME DESTINATION "." COMPONENT application)

# Hardening: include zlib1.dll (loaded via LoadLibrary() in openssl's libcrypto.a)
install(FILES "${ZLIB}" DESTINATION "." COMPONENT application)

if(WEBRTC_RUNTIME_DLL)
    install(FILES "${WEBRTC_RUNTIME_DLL}" DESTINATION "." COMPONENT application)
endif()

# NVIDIA TrueHDR runtime. Installer builds stage the pinned runtime at CPack
# time so RTX HDR cannot be shipped in a silently disabled state. Force the
# cache value on so older local build trees do not keep the previous optional
# default. Only the TrueHDR feature DLL is bundled; VSR is not used.
set(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME ON CACHE BOOL "Fail Windows packaging when the TrueHDR runtime DLLs are missing." FORCE)
set(SUNSHINE_TRUEHDR_RUNTIME_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Directory containing vibeshine_truehdr.dll and the NVIDIA NGX TrueHDR runtime DLL")
set(SUNSHINE_TRUEHDR_RUNTIME_FILES "")
foreach(_truehdr_runtime_name IN LISTS SUNSHINE_VDD_TRUEHDR_FILES)
    list(APPEND SUNSHINE_TRUEHDR_RUNTIME_FILES
        "${SUNSHINE_TRUEHDR_RUNTIME_DIR}/${_truehdr_runtime_name}")
endforeach()
unset(_truehdr_runtime_name)
set(SUNSHINE_TRUEHDR_RUNTIME_FILES_INSTALL_CODE "")
foreach(_truehdr_runtime_file IN LISTS SUNSHINE_TRUEHDR_RUNTIME_FILES)
    string(APPEND SUNSHINE_TRUEHDR_RUNTIME_FILES_INSTALL_CODE
        "        \"${_truehdr_runtime_file}\"\n")
endforeach()
unset(_truehdr_runtime_file)
if(SUNSHINE_REQUIRE_TRUEHDR_RUNTIME)
    set(_truehdr_stage_code [=[
execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "@CMAKE_SOURCE_DIR@/scripts/download_truehdr_runtime_release.ps1"
            -Repository "@SUNSHINE_VDD_TRUEHDR_REPOSITORY@"
            -Tag "@SUNSHINE_VDD_TRUEHDR_RELEASE_TAG@"
            -OutDir "@SUNSHINE_TRUEHDR_RUNTIME_DIR@"
    RESULT_VARIABLE _truehdr_stage_result
)
if(NOT _truehdr_stage_result EQUAL 0)
    message(FATAL_ERROR "Failed to stage required TrueHDR runtime files in @SUNSHINE_TRUEHDR_RUNTIME_DIR@")
endif()

foreach(_truehdr_runtime_file IN ITEMS
@SUNSHINE_TRUEHDR_RUNTIME_FILES_INSTALL_CODE@)
    if(NOT EXISTS "${_truehdr_runtime_file}")
        message(FATAL_ERROR "Required TrueHDR runtime file missing: ${_truehdr_runtime_file}")
    endif()
    file(SIZE "${_truehdr_runtime_file}" _truehdr_runtime_file_size)
    if(_truehdr_runtime_file_size EQUAL 0)
        message(FATAL_ERROR "Required TrueHDR runtime file is empty (0 bytes): ${_truehdr_runtime_file}")
    endif()
endforeach()
]=])
    string(CONFIGURE "${_truehdr_stage_code}" _truehdr_stage_code @ONLY)
    install(CODE "${_truehdr_stage_code}")
    unset(_truehdr_stage_code)
    install(FILES ${SUNSHINE_TRUEHDR_RUNTIME_FILES}
        DESTINATION "."
        COMPONENT application)
else()
    install(FILES ${SUNSHINE_TRUEHDR_RUNTIME_FILES}
        DESTINATION "."
        COMPONENT application
        OPTIONAL)
endif()

# ARM64: include minhook-detours DLL (shared library for ARM64)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64" AND DEFINED _MINHOOK_DLL)
    install(FILES "${_MINHOOK_DLL}" DESTINATION "." COMPONENT application)
endif()

# ViGEmBus installer is no longer bundled or managed by the installer

# Adding tools
install(TARGETS dxgi-info RUNTIME DESTINATION "tools" COMPONENT dxgi)
install(TARGETS audio-info RUNTIME DESTINATION "tools" COMPONENT audio)


# Helpers and tools
# - Playnite launcher helper used for Playnite-managed app launches
# - WGC capture helper used by the WGC display backend
# - Display helper used for applying/reverting display settings
if (TARGET playnite-launcher)
    install(TARGETS playnite-launcher RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_wgc_capture)
    install(TARGETS sunshine_wgc_capture RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET sunshine_display_helper)
    install(TARGETS sunshine_display_helper RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET steam_webhelper_proxy)
    install(TARGETS steam_webhelper_proxy RUNTIME DESTINATION "tools" COMPONENT application)
endif()
if (TARGET vibeshine_terminal_hdr_activator)
    install(TARGETS vibeshine_terminal_hdr_activator RUNTIME DESTINATION "tools" COMPONENT application)
endif()
install(FILES "${CMAKE_BINARY_DIR}/uninstall.exe" DESTINATION "." COMPONENT application)

# Drivers (SudoVDA virtual display)
set(SUDOVDA_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sudovda")
set(SUDOVDA_DRIVER_FILES
    "${SUDOVDA_SOURCE_DIR}/install.ps1"
    "${SUDOVDA_SOURCE_DIR}/uninstall.bat"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.inf"
    "${SUDOVDA_SOURCE_DIR}/SudoVDA.dll"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cat"
    "${SUDOVDA_SOURCE_DIR}/sudovda.cer"
    "${SUDOVDA_SOURCE_DIR}/nefconc.exe"
)

foreach(_sudovda_file IN LISTS SUDOVDA_DRIVER_FILES)
    if (NOT EXISTS "${_sudovda_file}")
        message(FATAL_ERROR "Required SudoVDA driver artifact missing: ${_sudovda_file}")
    endif()
    file(SIZE "${_sudovda_file}" _sudovda_file_size)
    if (_sudovda_file_size EQUAL 0)
        message(FATAL_ERROR "Required SudoVDA driver artifact is empty (0 bytes): ${_sudovda_file}")
    endif()
endforeach()
unset(_sudovda_file_size)
unset(_sudovda_file)

install(FILES ${SUDOVDA_DRIVER_FILES}
        DESTINATION "${SUNSHINE_VDD_SUDOVDA_DESTINATION}"
        COMPONENT sudovda)

# Drivers (Vibeshine Display Driver)
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/drivers/sunshine")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR
    "${CMAKE_BINARY_DIR}/packaging/windows/drivers/sunshine")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/refresh_driver_package.ps1")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/download_libvirtualdisplay_release.ps1")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_CACHE_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/stage_vdd_prebuilt_cache.ps1")
set(SUNSHINE_VDD_BUILD_MANIFEST_PATH "${CMAKE_BINARY_DIR}/libvirtualdisplay-manifest.json")
add_custom_command(
    OUTPUT "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SUNSHINE_VDD_MANIFEST_PATH}" "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
    DEPENDS "${SUNSHINE_VDD_MANIFEST_PATH}"
    COMMENT "Staging pinned virtual-display manifest")
add_custom_target(stage_sunshine_virtual_display_manifest
    DEPENDS "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}")

# The checked-in payload is the validation source.  Package installation must
# consume only the build-local refresh output so a package build cannot mutate
# or accidentally archive files from the source checkout.
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" _sunshine_vdd_binary_root)
file(TO_CMAKE_PATH "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}" _sunshine_vdd_source_dir)
file(TO_CMAKE_PATH "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}" _sunshine_vdd_package_dir)
string(REGEX REPLACE "/+$" "" _sunshine_vdd_binary_root "${_sunshine_vdd_binary_root}")
string(REGEX REPLACE "/+$" "" _sunshine_vdd_source_dir "${_sunshine_vdd_source_dir}")
string(REGEX REPLACE "/+$" "" _sunshine_vdd_package_dir "${_sunshine_vdd_package_dir}")
if("${_sunshine_vdd_package_dir}" STREQUAL "${_sunshine_vdd_binary_root}" OR
   "${_sunshine_vdd_package_dir}" STREQUAL "${_sunshine_vdd_source_dir}")
    message(FATAL_ERROR "Vibeshine Display Driver package directory must be distinct from the source checkout")
endif()
string(FIND "${_sunshine_vdd_package_dir}/" "${_sunshine_vdd_binary_root}/" _sunshine_vdd_package_root_index)
if(NOT _sunshine_vdd_package_root_index EQUAL 0)
    message(FATAL_ERROR "Vibeshine Display Driver package directory must be rooted under CMAKE_BINARY_DIR: ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}")
endif()
unset(_sunshine_vdd_package_root_index)
unset(_sunshine_vdd_package_dir)
unset(_sunshine_vdd_source_dir)
unset(_sunshine_vdd_binary_root)

set(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR "" CACHE PATH "Optional prebuilt libvirtualdisplay package root with driver/, tools/, and vulkan-layer/")
set(SUNSHINE_VDD_PREBUILT_CACHE_DIR
    "${CMAKE_BINARY_DIR}/libvirtualdisplay-release-${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}")
if(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR)
    file(TO_CMAKE_PATH "${SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR}" _sunshine_vdd_external_prebuilt)
    file(TO_CMAKE_PATH "${SUNSHINE_VDD_PREBUILT_CACHE_DIR}" _sunshine_vdd_cached_prebuilt)
    if(_sunshine_vdd_external_prebuilt STREQUAL _sunshine_vdd_cached_prebuilt)
        message(FATAL_ERROR "External virtual-display package must not be the build-local cache directory")
    endif()
    set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR "${SUNSHINE_VDD_PREBUILT_CACHE_DIR}")
    set(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE OFF)
else()
    set(SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR "${SUNSHINE_VDD_PREBUILT_CACHE_DIR}")
    set(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE ON)
endif()
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_FILES "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_STATIC_FILES "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES "")
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_OPTIONAL_FILES "")
foreach(_sunshine_driver_relative_file IN LISTS SUNSHINE_VDD_DRIVER_REQUIRED_FILES)
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_driver_relative_file}")
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}/${_sunshine_driver_relative_file}")
endforeach()
unset(_sunshine_driver_relative_file)
foreach(_sunshine_driver_static_name IN ITEMS install.ps1 nefconc.exe)
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_STATIC_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_driver_static_name}")
endforeach()
unset(_sunshine_driver_static_name)

set(SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_SOURCE_FILES "")
set(SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES "")
foreach(_sunshine_vulkan_relative_file IN LISTS SUNSHINE_VDD_VULKAN_LAYER_FILES)
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_SOURCE_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_vulkan_relative_file}")
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}/${_sunshine_vulkan_relative_file}")
endforeach()
unset(_sunshine_vulkan_relative_file)
set(SUNSHINE_VIRTUAL_DISPLAY_SOURCE_PACKAGE_FILES
    ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_FILES}
    ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_SOURCE_FILES}
)
foreach(_sunshine_driver_optional_name IN LISTS SUNSHINE_VDD_DRIVER_OPTIONAL_FILES)
    set(_sunshine_driver_optional_file
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}/${_sunshine_driver_optional_name}")
    if(EXISTS "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_FILES "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_STATIC_FILES "${_sunshine_driver_optional_file}")
        list(APPEND SUNSHINE_VIRTUAL_DISPLAY_SOURCE_PACKAGE_FILES "${_sunshine_driver_optional_file}")
    endif()
    list(APPEND SUNSHINE_VIRTUAL_DISPLAY_DRIVER_OPTIONAL_FILES
        "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}/${_sunshine_driver_optional_name}")
endforeach()
unset(_sunshine_driver_optional_file)
unset(_sunshine_driver_optional_name)

foreach(_sunshine_driver_file IN LISTS SUNSHINE_VIRTUAL_DISPLAY_SOURCE_PACKAGE_FILES)
    if (NOT EXISTS "${_sunshine_driver_file}")
        message(FATAL_ERROR "Required Vibeshine Display Driver artifact missing: ${_sunshine_driver_file}")
    endif()
    file(SIZE "${_sunshine_driver_file}" _sunshine_driver_file_size)
    if (_sunshine_driver_file_size EQUAL 0)
        message(FATAL_ERROR "Required Vibeshine Display Driver artifact is empty (0 bytes): ${_sunshine_driver_file}")
    endif()
endforeach()
unset(_sunshine_driver_file_size)
unset(_sunshine_driver_file)

if(EXISTS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}")
    if(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE)
        if(NOT EXISTS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}")
            message(FATAL_ERROR "Required libvirtualdisplay release downloader is missing: ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}")
        endif()
        add_custom_target(download_sunshine_virtual_display_driver_release
            COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                    -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}"
                    -Repository "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY}"
                    -Tag "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}"
                    -OutDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                    -ManifestPath "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
                    -TrustedBuildRoot "${CMAKE_BINARY_DIR}"
                    -SourceManifestPath "${SUNSHINE_VDD_MANIFEST_PATH}"
                    -SourceTrustedRoot "${CMAKE_SOURCE_DIR}"
            DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_DOWNLOAD_SCRIPT}"
                    stage_sunshine_virtual_display_manifest
            COMMENT "Downloading pinned Vibeshine Display Driver release"
            VERBATIM)
    endif()

    if(SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR)
        set(_sunshine_vdd_cache_marker "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}/.cache-ready")
        add_custom_command(OUTPUT "${_sunshine_vdd_cache_marker}"
            COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                    -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_CACHE_SCRIPT}"
                    -SourcePackageDir "${SUNSHINE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                    -PackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                    -ManifestPath "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
                    -TrustedBuildRoot "${CMAKE_BINARY_DIR}"
                    -SourceManifestPath "${SUNSHINE_VDD_MANIFEST_PATH}"
                    -SourceTrustedRoot "${CMAKE_SOURCE_DIR}"
            DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_CACHE_SCRIPT}"
                    "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
            COMMENT "Staging caller-provided virtual-display package into the build-local cache"
            VERBATIM)
        add_custom_target(stage_sunshine_virtual_display_driver_cache
            DEPENDS "${_sunshine_vdd_cache_marker}")
        unset(_sunshine_vdd_cache_marker)
    elseif(SUNSHINE_DOWNLOAD_LIBVIRTUALDISPLAY_RELEASE)
        add_custom_target(stage_sunshine_virtual_display_driver_cache)
        add_dependencies(stage_sunshine_virtual_display_driver_cache
            download_sunshine_virtual_display_driver_release)
    else()
        add_custom_target(stage_sunshine_virtual_display_driver_cache
            DEPENDS stage_sunshine_virtual_display_manifest)
    endif()

    add_custom_target(validate_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -ValidateOnly
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
                -ManifestPath "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
                -TrustedBuildRoot "${CMAKE_BINARY_DIR}"
                -SourceManifestPath "${SUNSHINE_VDD_MANIFEST_PATH}"
                -SourceTrustedRoot "${CMAKE_SOURCE_DIR}"
                -Repository "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY}"
                -Tag "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}"
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                stage_sunshine_virtual_display_manifest
                ${SUNSHINE_VIRTUAL_DISPLAY_SOURCE_PACKAGE_FILES}
        COMMENT "Validating Vibeshine Display Driver package assets"
        VERBATIM)

    add_dependencies(validate_sunshine_virtual_display_driver_assets
        stage_sunshine_virtual_display_driver_cache)

    add_custom_target(refresh_sunshine_virtual_display_driver_assets
        COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
                -File "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                -Build
                -LibVirtualDisplayDir "${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}"
                -PrebuiltPackageDir "${SUNSHINE_EFFECTIVE_LIBVIRTUALDISPLAY_PREBUILT_DIR}"
                -PackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR}"
                -ManifestPath "${SUNSHINE_VDD_BUILD_MANIFEST_PATH}"
                -TrustedBuildRoot "${CMAKE_BINARY_DIR}"
                -SourceManifestPath "${SUNSHINE_VDD_MANIFEST_PATH}"
                -SourceTrustedRoot "${CMAKE_SOURCE_DIR}"
                -SourcePackageDir "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR}"
                -Repository "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY}"
                -Tag "${SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG}"
                ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SIGNING_ARGS}
        DEPENDS "${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_REFRESH_SCRIPT}"
                stage_sunshine_virtual_display_manifest
                ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_STATIC_FILES}
        COMMENT "Refreshing Vibeshine Display Driver package assets from the pinned release"
        VERBATIM)

    add_dependencies(refresh_sunshine_virtual_display_driver_assets
        stage_sunshine_virtual_display_driver_cache)

    if(TARGET package_msi AND SUNSHINE_VDD_REFRESH_BEFORE_MSI)
        add_dependencies(package_msi refresh_sunshine_virtual_display_driver_assets)
    endif()
endif()

# Guard the CPack destination before any VDD install(FILES) operation. This
# validates the source-pinned ready record and rejects a reparse/interposed
# destination before CPack can create or copy into it.
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_INSTALL_VALIDATION_SCRIPT
    "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/validate_vdd_install.ps1")
set(_sunshine_vdd_install_preflight_code [=[
execute_process(
    COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
            -File "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_INSTALL_VALIDATION_SCRIPT@"
            -ManifestPath "@SUNSHINE_VDD_BUILD_MANIFEST_PATH@"
            -SourceManifestPath "@SUNSHINE_VDD_MANIFEST_PATH@"
            -SourceTrustedRoot "@CMAKE_SOURCE_DIR@"
            -SourcePackageRoot "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR@"
            -PackageRoot "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR@"
            -InstalledPackageRoot "${CMAKE_INSTALL_PREFIX}/@SUNSHINE_VDD_DRIVER_DESTINATION@"
            -TrustedBuildRoot "@CMAKE_BINARY_DIR@"
            -Preflight -RequirePinnedProvenance
    RESULT_VARIABLE _sunshine_vdd_preflight_result)
if(NOT _sunshine_vdd_preflight_result EQUAL 0)
    message(FATAL_ERROR "Pinned virtual-display pre-install validation failed.")
endif()
]=])
string(CONFIGURE "${_sunshine_vdd_install_preflight_code}" _sunshine_vdd_install_preflight_code @ONLY)
install(CODE "${_sunshine_vdd_install_preflight_code}" COMPONENT virtual_display_driver)
unset(_sunshine_vdd_install_preflight_code)

install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_FILES}
        DESTINATION "${SUNSHINE_VDD_DRIVER_DESTINATION}"
        COMPONENT virtual_display_driver)
install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_DRIVER_OPTIONAL_FILES}
        DESTINATION "${SUNSHINE_VDD_DRIVER_DESTINATION}"
        COMPONENT virtual_display_driver
        OPTIONAL)
install(FILES ${SUNSHINE_VIRTUAL_DISPLAY_VULKAN_LAYER_FILES}
        DESTINATION "${SUNSHINE_VDD_VULKAN_LAYER_DESTINATION}"
        COMPONENT virtual_display_driver)

# CPack runs the install script independently of the package_msi target graph.
# Validate both the build-local published payload and the exact files copied
# into the CPack staging tree, and require the pinned-release provenance record.
set(_sunshine_vdd_install_validation_code [=[
execute_process(
    COMMAND powershell -NoLogo -NonInteractive -NoProfile -ExecutionPolicy Bypass
            -File "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_INSTALL_VALIDATION_SCRIPT@"
            -ManifestPath "@SUNSHINE_VDD_BUILD_MANIFEST_PATH@"
            -SourceManifestPath "@SUNSHINE_VDD_MANIFEST_PATH@"
            -SourceTrustedRoot "@CMAKE_SOURCE_DIR@"
            -SourcePackageRoot "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_SOURCE_DIR@"
            -PackageRoot "@SUNSHINE_VIRTUAL_DISPLAY_DRIVER_PACKAGE_DIR@"
            -InstalledPackageRoot "${CMAKE_INSTALL_PREFIX}/@SUNSHINE_VDD_DRIVER_DESTINATION@"
            -TrustedBuildRoot "@CMAKE_BINARY_DIR@"
            -RequirePinnedProvenance
    RESULT_VARIABLE _sunshine_vdd_install_validation_result)
if(NOT _sunshine_vdd_install_validation_result EQUAL 0)
    message(FATAL_ERROR "Pinned virtual-display install validation failed.")
endif()
]=])
set(SUNSHINE_VIRTUAL_DISPLAY_DRIVER_INSTALL_VALIDATION_SCRIPT
    "${CMAKE_SOURCE_DIR}/packaging/windows/virtual_display_driver/validate_vdd_install.ps1")
string(CONFIGURE "${_sunshine_vdd_install_validation_code}" _sunshine_vdd_install_validation_code @ONLY)
install(CODE "${_sunshine_vdd_install_validation_code}" COMPONENT virtual_display_driver)
unset(_sunshine_vdd_install_validation_code)

# Mandatory scripts
install(FILES "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/sunshine-setup.ps1"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/service/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/migration/"
        DESTINATION "scripts"
        COMPONENT assets)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/path/"
        DESTINATION "scripts"
        COMPONENT assets)

# Configurable options for the service
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/autostart/"
        DESTINATION "scripts"
        COMPONENT autostart)

# scripts
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/firewall/"
        DESTINATION "scripts"
        COMPONENT firewall)
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/misc/gamepad/"
        DESTINATION "scripts"
        COMPONENT assets)

# Sunshine assets
install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}"
        COMPONENT assets)

# Plugins (copy plugin folders such as `plugins/playnite` into the package)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/plugins/"
        DESTINATION "plugins"
        COMPONENT assets)

# Experimental native RDP-Tcp terminal isolation is independently gated and
# contributes no files unless an explicit vetted asset directory was supplied.
include("${CMAKE_SOURCE_DIR}/cmake/packaging/windows_terminal_isolation.cmake")

# copy assets (excluding shaders) to build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets"
        PATTERN "shaders" EXCLUDE)

if(WEBRTC_RUNTIME_DLL)
    file(COPY "${WEBRTC_RUNTIME_DLL}"
            DESTINATION "${CMAKE_BINARY_DIR}")
endif()
# use junction for shaders directory
cmake_path(CONVERT "${SUNSHINE_SOURCE_ASSETS_DIR}/windows/assets/shaders"
        TO_NATIVE_PATH_LIST shaders_in_build_src_native)
cmake_path(CONVERT "${CMAKE_BINARY_DIR}/assets/shaders" TO_NATIVE_PATH_LIST shaders_in_build_dest_native)
if(NOT EXISTS "${CMAKE_BINARY_DIR}/assets/shaders")
    execute_process(COMMAND cmd.exe /c mklink /J "${shaders_in_build_dest_native}" "${shaders_in_build_src_native}")
endif()

set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}\\\\sunshine.ico")

# The name of the directory that will be created in C:/Program files/
# Keep install directory as Sunshine regardless of displayed product name
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Sunshine")

# Setting components groups and dependencies
set(CPACK_COMPONENT_GROUP_CORE_EXPANDED true)
set(CPACK_COMPONENT_GROUP_THIRDPARTY_DISPLAY_NAME "Third Party")
set(CPACK_COMPONENT_GROUP_THIRDPARTY_DESCRIPTION "Bundled third-party installers and optional components.")

# sunshine binary
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_COMPONENT_APPLICATION_DESCRIPTION "${CMAKE_PROJECT_NAME} main application and required components.")
set(CPACK_COMPONENT_APPLICATION_GROUP "Core")
set(CPACK_COMPONENT_APPLICATION_REQUIRED true)
set(CPACK_COMPONENT_APPLICATION_DEPENDS assets)

# service auto-start script
set(CPACK_COMPONENT_AUTOSTART_DISPLAY_NAME "Launch on Startup")
set(CPACK_COMPONENT_AUTOSTART_DESCRIPTION "If enabled, launches Vibeshine automatically on system startup.")
set(CPACK_COMPONENT_AUTOSTART_GROUP "Core")

# assets
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Required Assets")
set(CPACK_COMPONENT_ASSETS_DESCRIPTION "Shaders, default box art, and configuration-server assets.")
set(CPACK_COMPONENT_ASSETS_GROUP "Core")
set(CPACK_COMPONENT_ASSETS_REQUIRED true)

# drivers
set(CPACK_COMPONENT_SUDOVDA_DISPLAY_NAME "SudoVDA")
set(CPACK_COMPONENT_SUDOVDA_DESCRIPTION "Bundled rollback virtual display driver.")
set(CPACK_COMPONENT_SUDOVDA_GROUP "Drivers")
set(CPACK_COMPONENT_SUDOVDA_REQUIRED true)

set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DISPLAY_NAME "Vibeshine Display Driver")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_DESCRIPTION "Default virtual display driver.")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_GROUP "Drivers")
set(CPACK_COMPONENT_VIRTUAL_DISPLAY_DRIVER_REQUIRED true)

# audio tool
set(CPACK_COMPONENT_AUDIO_DISPLAY_NAME "audio-info")
set(CPACK_COMPONENT_AUDIO_DESCRIPTION "CLI tool providing information about sound devices.")
set(CPACK_COMPONENT_AUDIO_GROUP "Tools")

# display tool
set(CPACK_COMPONENT_DXGI_DISPLAY_NAME "dxgi-info")
set(CPACK_COMPONENT_DXGI_DESCRIPTION "CLI tool providing information about graphics cards and displays.")
set(CPACK_COMPONENT_DXGI_GROUP "Tools")

# firewall scripts
set(CPACK_COMPONENT_FIREWALL_DISPLAY_NAME "Add Firewall Exclusions")
set(CPACK_COMPONENT_FIREWALL_DESCRIPTION "Scripts to enable or disable firewall rules.")
set(CPACK_COMPONENT_FIREWALL_GROUP "Scripts")

# gamepad scripts are bundled under assets and not exposed as a separate component

# include specific packaging (WiX only)
include(${CMAKE_MODULE_PATH}/packaging/windows_wix.cmake)
