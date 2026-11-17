# The source contribution does not grant the pending USB product identity.
option(SUNSHINE_ENABLE_VIBE_SPEAKERS "Build the managed Vibe Speakers USB endpoint" OFF)
option(SUNSHINE_VIBE_PRIVATE_IDENTITY "Explicitly acknowledge pending 1209:7140; private tests only" OFF)
if(SUNSHINE_ENABLE_VIBE_SPEAKERS)
    if(NOT WIN32 OR NOT SUNSHINE_VIBE_PRIVATE_IDENTITY)
        message(FATAL_ERROR "Vibe Speakers requires Windows and explicit SUNSHINE_VIBE_PRIVATE_IDENTITY=ON until the USB identity is assigned")
    endif()
    target_compile_definitions(sunshine PRIVATE SUNSHINE_ENABLE_VIBE_SPEAKERS=1)
    set(vibe_root "${CMAKE_SOURCE_DIR}/src/vibe-speakers")
    add_library(vibe_usb_transport STATIC
        "${vibe_root}/vibe-usb-audio/src/protocol.cpp"
        "${vibe_root}/vibe-usb-audio/src/clock_evidence.cpp"
        "${vibe_root}/vibe-usb-audio/src/transport.cpp")
    target_compile_features(vibe_usb_transport PUBLIC cxx_std_20)
    target_include_directories(vibe_usb_transport PUBLIC
        "${vibe_root}/vibe-usb-audio/include" "${vibe_root}/vibe-usb-bridge/include")
    target_compile_definitions(vibe_usb_transport PUBLIC UNICODE _UNICODE NOMINMAX)
    target_link_libraries(vibe_usb_transport PUBLIC ws2_32 cfgmgr32 ole32 propsys uuid bcrypt advapi32 avrt)
    add_executable(VibeUsbBridge "${vibe_root}/vibe-usb-bridge/src/bridge_main.cpp")
    target_link_libraries(VibeUsbBridge PRIVATE vibe_usb_transport)
    set_target_properties(VibeUsbBridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
    if(MINGW)
        target_link_options(VibeUsbBridge PRIVATE -municode -static)
    endif()
    add_dependencies(sunshine VibeUsbBridge)
    add_executable(VibeUsbBridgeTests EXCLUDE_FROM_ALL "${vibe_root}/vibe-usb-bridge/tests/bridge_tests.cpp")
    target_link_libraries(VibeUsbBridgeTests PRIVATE vibe_usb_transport)
    if(MINGW)
        target_link_options(VibeUsbBridgeTests PRIVATE -static)
    endif()
    file(COPY "${CMAKE_SOURCE_DIR}/packaging/windows/vibe-speakers/" DESTINATION "${CMAKE_BINARY_DIR}/VibeSpeakers")
    # Explicit local staging only; never part of CPack or an upload workflow.
    add_custom_target(vibe-speakers-private-stage
        COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned
            -File "${CMAKE_SOURCE_DIR}/scripts/download_usbip_transport.ps1"
            -OutDir "${CMAKE_BINARY_DIR}/drivers/usbip-win2"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/windows/usbip_transport/LICENSE.txt"
            "${CMAKE_BINARY_DIR}/drivers/usbip-win2/LICENSE.txt"
        COMMAND powershell.exe -NoProfile -ExecutionPolicy RemoteSigned
            -File "${CMAKE_SOURCE_DIR}/packaging/windows/vibe-speakers/Write-PayloadLock.ps1"
            -PackageRoot "${CMAKE_BINARY_DIR}"
        DEPENDS sunshine VibeUsbBridge
        VERBATIM)
    set(CPACK_VIBE_PRIVATE_IDENTITY ON)
else()
    target_compile_definitions(sunshine PRIVATE SUNSHINE_ENABLE_VIBE_SPEAKERS=0)
endif()
