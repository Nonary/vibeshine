cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED CMAKE_CUDA_STANDARD)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

include_directories(
        "${CMAKE_SOURCE_DIR}"
)

set(TOOL_SOURCES
        "${CMAKE_SOURCE_DIR}/src/logging.cpp"
        "${CMAKE_SOURCE_DIR}/src/logging_policy.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp"
)

add_executable(dxgi-info dxgi.cpp ${TOOL_SOURCES})
target_link_libraries(dxgi-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES}
)
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp ${TOOL_SOURCES})
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp
        ${CMAKE_SOURCE_DIR}/src/crypto.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/terminal_session_seat_provider.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_broker.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_launch_codec.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_runtime.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_display.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_service.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_seat_pool.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_worker.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_worker_process.cpp
        ${CMAKE_SOURCE_DIR}/src/steam_offline_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/steam_offline_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp)
target_link_libraries(sunshinesvc
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        netapi32
        userenv
        fwpuclnt
        advapi32
        shell32
        user32
        libvirtualdisplay::driver
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_custom_command(TARGET sunshinesvc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:sunshinesvc>" "${CMAKE_BINARY_DIR}/wix_payload/tools/sunshinesvc.exe"
)

# Hidden inbox-RDP ActiveX host used by the privileged terminal seat broker.
# Credentials cross only a protected inherited named-pipe channel; they never
# appear in the process command line, environment, registry, or a .rdp file.
find_program(VIBESHINE_CSC_EXECUTABLE csc.exe
        PATHS "$ENV{WINDIR}/Microsoft.NET/Framework64/v4.0.30319"
              "$ENV{WINDIR}/Microsoft.NET/Framework/v4.0.30319"
        NO_DEFAULT_PATH)
if(VIBESHINE_CSC_EXECUTABLE)
    set(TERMINAL_SEAT_CONTROLLER_EXE "${CMAKE_CURRENT_BINARY_DIR}/vibeshine_seat_controller.exe")
    add_custom_command(
            OUTPUT "${TERMINAL_SEAT_CONTROLLER_EXE}"
            COMMAND "${VIBESHINE_CSC_EXECUTABLE}" /nologo /target:winexe /platform:x64 /optimize+
                    /reference:System.dll /reference:System.Core.dll
                    /reference:System.Windows.Forms.dll /reference:System.Drawing.dll
                    "/out:${TERMINAL_SEAT_CONTROLLER_EXE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/terminal_seat_controller.cs"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/terminal_seat_controller.cs"
            VERBATIM)
    add_custom_target(vibeshine_seat_controller ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/wix_payload/tools"
            COMMAND ${CMAKE_COMMAND} -E copy "${TERMINAL_SEAT_CONTROLLER_EXE}" "${CMAKE_BINARY_DIR}/wix_payload/tools/vibeshine_seat_controller.exe"
            DEPENDS "${TERMINAL_SEAT_CONTROLLER_EXE}")
    add_dependencies(sunshinesvc vibeshine_seat_controller)
else()
    message(FATAL_ERROR "The .NET Framework C# compiler is required for the Vibeshine terminal seat controller")
endif()

add_executable(sunshine_terminal_worker_probe
        # Internal admission/IPC test host. This is intentionally not a
        # user-facing Sunshine worker and is not installed by the MSI.
        terminal_worker_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/utf_utils.cpp)
target_link_libraries(sunshine_terminal_worker_probe PRIVATE ${Boost_LIBRARIES} OpenSSL::Crypto wtsapi32 ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_terminal_worker_probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Ordinary user-mode proxy for a mirrored Steam webhelper.  It rewrites CEF
# cache/profile arguments and launches the renamed real helper by exact path;
# no injection, IFEO, DLL patching, or AppContainer is involved.
add_executable(steam_webhelper_proxy steam_webhelper_proxy.cpp)
set_target_properties(steam_webhelper_proxy PROPERTIES WIN32_EXECUTABLE TRUE CXX_STANDARD 23)
target_link_libraries(steam_webhelper_proxy PRIVATE shell32 advapi32)
target_compile_options(steam_webhelper_proxy PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Disposable seat-local helper for the proven FP16 cloned-primary,
# PreserveVidPn=FALSE D3DKMT HDR activation transaction. The SYSTEM broker
# owns launch and watchdog; keeping graphics calls in this child protects
# Terminal Services callbacks.
add_executable(vibeshine_terminal_hdr_activator terminal_hdr_activator.cpp)
set_target_properties(vibeshine_terminal_hdr_activator PROPERTIES CXX_STANDARD 23)
target_link_libraries(vibeshine_terminal_hdr_activator PRIVATE d3d11 dxgi gdi32 user32 wtsapi32 advapi32)
target_compile_options(vibeshine_terminal_hdr_activator PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Playnite launcher: per-launch helper that hosts a private IPC server
add_executable(playnite-launcher
        playnite_launcher/main.cpp
        playnite_launcher/launcher.cpp
        playnite_launcher/arguments.cpp
        playnite_launcher/cleanup.cpp
        playnite_launcher/focus_utils.cpp
        playnite_launcher/lossless_scaling.cpp
        playnite_launcher/lossless_scaling_policy.cpp
        playnite_launcher/playnite_process.cpp)
set_target_properties(playnite-launcher PROPERTIES CXX_STANDARD 23)
set_target_properties(playnite-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_include_directories(playnite-launcher PRIVATE ${CMAKE_SOURCE_DIR}/tools/playnite_launcher)
target_sources(playnite-launcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_ipc.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/playnite_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp)
target_compile_definitions(playnite-launcher PRIVATE SUNSHINE_PLAYNITE_LAUNCHER)
target_link_libraries(playnite-launcher
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        nlohmann_json::nlohmann_json
        user32
        userenv
        ole32
        oleaut32
        Psapi
        shell32
        shlwapi
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(playnite-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
# Add sunshine_wgc_capture executable
add_executable(sunshine_wgc_capture
        sunshine_wgc_capture.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp)
set_target_properties(sunshine_wgc_capture PROPERTIES CXX_STANDARD 23)

# Define helper build flag to conditionally compile logging
target_compile_definitions(sunshine_wgc_capture PRIVATE
        SUNSHINE_EXTERNAL_PROCESS=1
)

target_link_libraries(sunshine_wgc_capture
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        d3d11
        dxgi
        ole32
        user32
        gdi32
        windowsapp
        avrt
        shcore
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_wgc_capture PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Display settings helper: detached helper that applies/reverts display settings via IPC.
# One exe hosts both engines (legacy + v2), selected by --engine= or persisted state.
add_executable(sunshine_display_helper
        display_settings_helper_main.cpp
        display_settings_helper.cpp
        display_settings_helper_v2.cpp
        ${CMAKE_SOURCE_DIR}/src/terminal_session_display.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/diagnostics.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/diagnostics_boost.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/file_text_storage.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/topology_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/async_dispatcher.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/golden_health.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/operations.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot_codec.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/snapshot_file_storage.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/state_machine.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_display_settings.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_event_pump.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_platform_workarounds.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_shell_refresh_policy.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/display_helper_v2/win_scheduled_task_manager.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/pipes.cpp
        ${CMAKE_SOURCE_DIR}/src/platform/windows/ipc/misc_utils.cpp
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
        ${CMAKE_SOURCE_DIR}/src/logging_policy.cpp)
set_target_properties(sunshine_display_helper PROPERTIES CXX_STANDARD 23)
target_compile_definitions(sunshine_display_helper PRIVATE SETUP_LIBDISPLAYDEVICE_LOGGING)
target_include_directories(sunshine_display_helper PRIVATE ${CMAKE_SOURCE_DIR}/third-party)
target_link_libraries(sunshine_display_helper
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        libdisplaydevice::display_device
        libvirtualdisplay::driver
        user32
        wtsapi32
        advapi32
        secur32
        ole32
        taskschd
        ${OPENSSL_LIBRARIES}
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshine_display_helper PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
