cmake_minimum_required(VERSION 3.20)

project(sunshine_tools)

include_directories("${CMAKE_SOURCE_DIR}")

add_executable(dxgi-info dxgi.cpp)
set_target_properties(dxgi-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(dxgi-info
        ${CMAKE_THREAD_LIBS_INIT}
        dxgi
        ${PLATFORM_LIBRARIES})
target_compile_options(dxgi-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(audio-info audio.cpp)
set_target_properties(audio-info PROPERTIES CXX_STANDARD 23)
target_link_libraries(audio-info
        ${Boost_LIBRARIES}
        ${CMAKE_THREAD_LIBS_INIT}
        ksuser
        ${PLATFORM_LIBRARIES})
target_compile_options(audio-info PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

add_executable(sunshinesvc sunshinesvc.cpp)
set_target_properties(sunshinesvc PROPERTIES CXX_STANDARD 23)
target_link_libraries(sunshinesvc
        ${CMAKE_THREAD_LIBS_INIT}
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(sunshinesvc PRIVATE ${SUNSHINE_COMPILE_OPTIONS})

# Playnite launcher: per-launch helper that hosts a private IPC server
add_executable(playnite-launcher playnite_launcher.cpp)
set_target_properties(playnite-launcher PROPERTIES CXX_STANDARD 23)
set_target_properties(playnite-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
target_sources(playnite-launcher PRIVATE
        ${CMAKE_SOURCE_DIR}/src/logging.cpp
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
        Psapi
        shell32
        wtsapi32
        ${PLATFORM_LIBRARIES})
target_compile_options(playnite-launcher PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
