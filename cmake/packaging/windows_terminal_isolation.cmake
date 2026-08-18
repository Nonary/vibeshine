# Experimental native RDP-Tcp terminal-isolation payload.
#
# This file intentionally has no fallback assets.  Enabling the build option
# requires a caller-provided directory and exact compatibility pins.  These
# are test-only pins for the known TermWrap/Zydis pair; a self-describing
# manifest is not trusted as provenance.

if(NOT WIN32 OR NOT SUNSHINE_ENABLE_TERMINAL_ISOLATION)
    return()
endif()

if(NOT IS_DIRECTORY "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}")
    message(FATAL_ERROR
        "SUNSHINE_ENABLE_TERMINAL_ISOLATION requires "
        "-DSUNSHINE_TERMINAL_ISOLATION_ASSET_DIR=<vetted asset directory>")
endif()
set(SUNSHINE_TERMINAL_ISOLATION_TERMSRV_SHA256
    "f2d3150c45e3fe5dbf294abc6ed8326d6d36936fdff256f7e805cff55f9b1aea")
set(SUNSHINE_TERMINAL_ISOLATION_TERMSRV_FILE_VERSION "10.0.26100.8115")
file(READ "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal-isolation.ps1" _terminal_isolation_script_text)
if(NOT _terminal_isolation_script_text MATCHES "${SUNSHINE_TERMINAL_ISOLATION_TERMSRV_SHA256}" OR
   NOT _terminal_isolation_script_text MATCHES "${SUNSHINE_TERMINAL_ISOLATION_TERMSRV_FILE_VERSION}")
    message(FATAL_ERROR "The reviewed termsrv pin and terminal-isolation.ps1 are inconsistent")
endif()

# Do not turn these into cache variables: changing the expected hash must be a
# source-reviewed change.  These are exact test-only pins for the known
# TermWrap compatibility pair and its license.
set(_terminal_isolation_termwrap_sha256
    "220F18E0B2C2091C5F684EC063C43831BFFDF25E561BD123211CCE883F8D25E2")
set(_terminal_isolation_zydis_sha256
    "5908BE0AF05BF7584328CF5D0DDDE2C108D693709FFC77A13822FDCEB75797E1")
set(_terminal_isolation_license_sha256
    "72966F08CEAACF34475E7824AC566F2E966BEF3C5E46A190DC844C1155486614")

set(_terminal_isolation_asset_names
    TermWrap.dll
    Zydis.dll
    LICENSE
    terminal-isolation.ps1
    status-contract.txt)
set(_terminal_isolation_asset_sources
    "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/TermWrap.dll"
    "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/Zydis.dll"
    "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal-isolation.ps1"
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/status-contract.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_terminal_isolation_asset_sources}
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/manifest.json.in"
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal_isolation_ca.cpp"
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal_isolation_ca_config.h.in"
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/verify_terminal_isolation_ca_imports.cmake")
set(_terminal_isolation_asset_paths)
set(_terminal_isolation_manifest_entries)
set(_terminal_isolation_manifest_first TRUE)
list(LENGTH _terminal_isolation_asset_names _terminal_isolation_asset_count)
math(EXPR _terminal_isolation_asset_last "${_terminal_isolation_asset_count} - 1")
foreach(_asset_index RANGE ${_terminal_isolation_asset_last})
    list(GET _terminal_isolation_asset_names ${_asset_index} _asset_name)
    list(GET _terminal_isolation_asset_sources ${_asset_index} _asset_path)
    if(NOT IS_REGULAR_FILE "${_asset_path}")
        message(FATAL_ERROR "Missing terminal-isolation asset: ${_asset_path}")
    endif()
    file(SIZE "${_asset_path}" _asset_size)
    if(_asset_size LESS 1)
        message(FATAL_ERROR "Empty terminal-isolation asset: ${_asset_path}")
    endif()
    file(SHA256 "${_asset_path}" _asset_hash)
    set(_pinned_hash "")
    if(_asset_index EQUAL 0)
        set(_pinned_hash "${_terminal_isolation_termwrap_sha256}")
    elseif(_asset_index EQUAL 1)
        set(_pinned_hash "${_terminal_isolation_zydis_sha256}")
    elseif(_asset_index EQUAL 2)
        set(_pinned_hash "${_terminal_isolation_license_sha256}")
    endif()
    if(NOT _pinned_hash STREQUAL "")
        string(TOLOWER "${_asset_hash}" _asset_hash_lower)
        string(TOLOWER "${_pinned_hash}" _pinned_hash_lower)
        if(NOT _asset_hash_lower STREQUAL _pinned_hash_lower)
            message(FATAL_ERROR
                "TEST ONLY terminal-isolation asset pin mismatch for ${_asset_name}: "
                "expected ${_pinned_hash}, got ${_asset_hash}")
        endif()
    endif()
    if(NOT _terminal_isolation_manifest_first)
        string(APPEND _terminal_isolation_manifest_entries ",\n")
    endif()
    string(APPEND _terminal_isolation_manifest_entries
        "    {\"path\": \"${_asset_name}\", \"sha256\": \"${_asset_hash}\"}")
    set(_terminal_isolation_manifest_first FALSE)
    list(APPEND _terminal_isolation_asset_paths "${_asset_path}")
endforeach()
string(APPEND _terminal_isolation_manifest_entries "\n")

file(SHA256
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal-isolation.ps1"
    TERMINAL_ISOLATION_SCRIPT_SHA256)
string(TOLOWER "${TERMINAL_ISOLATION_SCRIPT_SHA256}" TERMINAL_ISOLATION_SCRIPT_SHA256)
set(_terminal_isolation_ca_config
    "${CMAKE_CURRENT_BINARY_DIR}/terminal_isolation_ca_config.h")
configure_file(
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal_isolation_ca_config.h.in"
    "${_terminal_isolation_ca_config}" @ONLY)

# The native CA is the immutable MSI-to-PowerShell boundary.  It is present
# only in an explicitly enabled test package and is staged beside the
# manifest-bound helper below.
add_library(TerminalIsolationCA SHARED
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal_isolation_ca.cpp"
    "${_terminal_isolation_ca_config}")
target_include_directories(TerminalIsolationCA PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(TerminalIsolationCA PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(TerminalIsolationCA PRIVATE msi advapi32 bcrypt)
target_compile_options(TerminalIsolationCA PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
if(MINGW)
    # The MSI Binary stream must be loadable before the application payload is
    # present.  Keep GCC, libstdc++, and winpthreads out of its dependency set.
    target_link_options(TerminalIsolationCA PRIVATE -static -static-libgcc -static-libstdc++)
elseif(MSVC)
    # Keep the Binary-table DLL independent of the machine's VC runtime.
    set_property(TARGET TerminalIsolationCA PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(CMAKE_OBJDUMP)
    set(_terminal_isolation_import_tool "${CMAKE_OBJDUMP}")
    set(_terminal_isolation_import_mode objdump)
else()
    find_program(_terminal_isolation_import_tool NAMES dumpbin)
    if(_terminal_isolation_import_tool)
        set(_terminal_isolation_import_mode dumpbin)
    else()
        message(FATAL_ERROR
            "SUNSHINE_ENABLE_TERMINAL_ISOLATION requires CMAKE_OBJDUMP or dumpbin "
            "to enforce the standalone TerminalIsolationCA import gate")
    endif()
endif()

set(_terminal_isolation_manifest
    "${CMAKE_CURRENT_BINARY_DIR}/terminal-isolation-manifest.json")
set(TERMINAL_ISOLATION_MANIFEST_ENTRIES "${_terminal_isolation_manifest_entries}")
configure_file(
    "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/manifest.json.in"
    "${_terminal_isolation_manifest}" @ONLY)

set(_terminal_isolation_payload_dir "${CMAKE_BINARY_DIR}/wix_payload/terminal-isolation")
file(MAKE_DIRECTORY "${_terminal_isolation_payload_dir}")
add_custom_target(terminal_isolation_payload
    DEPENDS TerminalIsolationCA
        ${_terminal_isolation_asset_sources}
        "${_terminal_isolation_manifest}"
        "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/verify_terminal_isolation_ca_imports.cmake"
    COMMAND "${CMAKE_COMMAND}"
        "-DTERMINAL_ISOLATION_CA_DLL=$<TARGET_FILE:TerminalIsolationCA>"
        "-DTERMINAL_ISOLATION_IMPORT_TOOL=${_terminal_isolation_import_tool}"
        "-DTERMINAL_ISOLATION_IMPORT_MODE=${_terminal_isolation_import_mode}"
        -P "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/verify_terminal_isolation_ca_imports.cmake"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:TerminalIsolationCA>"
        "${_terminal_isolation_payload_dir}/TerminalIsolationCA.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${_terminal_isolation_manifest}"
        "${_terminal_isolation_payload_dir}/terminal-isolation-manifest.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/terminal-isolation.ps1"
        "${_terminal_isolation_payload_dir}/terminal-isolation.ps1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/packaging/windows/terminal_isolation/status-contract.txt"
        "${_terminal_isolation_payload_dir}/status-contract.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/TermWrap.dll"
        "${_terminal_isolation_payload_dir}/TermWrap.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/Zydis.dll"
        "${_terminal_isolation_payload_dir}/Zydis.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SUNSHINE_TERMINAL_ISOLATION_ASSET_DIR}/LICENSE"
        "${_terminal_isolation_payload_dir}/LICENSE"
    VERBATIM)
if(TARGET package_msi)
    add_dependencies(package_msi terminal_isolation_payload)
endif()

unset(_terminal_isolation_asset_name)
unset(_terminal_isolation_asset_names)
unset(_terminal_isolation_termwrap_sha256)
unset(_terminal_isolation_zydis_sha256)
unset(_terminal_isolation_license_sha256)
unset(_terminal_isolation_asset_sources)
unset(_terminal_isolation_asset_paths)
unset(_terminal_isolation_manifest_entries)
unset(_terminal_isolation_manifest_first)
unset(_terminal_isolation_manifest)
unset(_terminal_isolation_payload_dir)
unset(_terminal_isolation_script_text)
unset(_terminal_isolation_ca_config)
unset(TERMINAL_ISOLATION_SCRIPT_SHA256)
