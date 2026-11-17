cmake_minimum_required(VERSION 3.24)
project(VibeshineHost714Tests LANGUAGES C CXX)
enable_testing()
add_test(NAME vibe_public_endpoint_package_guard COMMAND "${CMAKE_COMMAND}"
    "-DGUARD_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/packaging/check_vibe_public_identity.cmake"
    "-DGUARD_TEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/public_package_guard.cmake")
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(HOST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(COMMON_SOURCE_DIR "" CACHE PATH "Pinned completed 7.1.4 Common source, read only")
option(HOST714_BUILD_TRANSPORT "Build real codec/packet/receiver integration" ON)
option(HOST714_SANITIZERS "Build portable regression tests with address/undefined sanitizers" OFF)
if(HOST714_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MINGW)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(FATAL_ERROR "This combined sanitizer configuration needs a supported native GCC/Clang runtime; do not count it as a Windows PASS")
    endif()
endif()
if(NOT EXISTS "${COMMON_SOURCE_DIR}/src/AudioConfig.c")
    message(FATAL_ERROR "COMMON_SOURCE_DIR must identify the completed client Common checkout with AudioConfig.c")
endif()

add_executable(Host714ProtocolTests protocol_tests.cpp
    "${HOST_ROOT}/src/audio_protocol.cpp" "${HOST_ROOT}/src/audio_policy.cpp"
    "${COMMON_SOURCE_DIR}/src/AudioConfig.c")
target_include_directories(Host714ProtocolTests PRIVATE "${HOST_ROOT}" "${COMMON_SOURCE_DIR}/src")
add_test(NAME host714_protocol COMMAND Host714ProtocolTests)
find_package(Threads REQUIRED)
add_executable(Host714DeliveryTests delivery_tests.cpp)
target_include_directories(Host714DeliveryTests PRIVATE "${HOST_ROOT}")
target_link_libraries(Host714DeliveryTests PRIVATE Threads::Threads)
add_test(NAME host714_delivery COMMAND Host714DeliveryTests)
set_tests_properties(host714_delivery PROPERTIES TIMEOUT 20)
add_executable(Host714SourceLifecycleTests source_lifecycle_tests.cpp)
target_include_directories(Host714SourceLifecycleTests PRIVATE "${HOST_ROOT}")
target_link_libraries(Host714SourceLifecycleTests PRIVATE Threads::Threads)
add_test(NAME host714_source_lifecycle COMMAND Host714SourceLifecycleTests)
set_tests_properties(host714_source_lifecycle PROPERTIES TIMEOUT 20)
add_executable(VibeUsbSourceTests usb_source_tests.cpp)
add_executable(VibeUsbBroadcastQueueTests usb_broadcast_queue_tests.cpp)
target_include_directories(VibeUsbBroadcastQueueTests PRIVATE "${HOST_ROOT}")
target_link_libraries(VibeUsbBroadcastQueueTests PRIVATE Threads::Threads)
if(MINGW)
    target_link_options(VibeUsbBroadcastQueueTests PRIVATE -static-libgcc -static-libstdc++)
endif()
add_test(NAME vibe_usb_broadcast_queue_generation COMMAND VibeUsbBroadcastQueueTests)
target_include_directories(VibeUsbSourceTests PRIVATE "${HOST_ROOT}")
add_test(NAME vibe_usb_source_validation COMMAND VibeUsbSourceTests)
if(MINGW)
    target_link_options(VibeUsbSourceTests PRIVATE -static-libgcc -static-libstdc++)
endif()
if(MSVC)
    target_compile_options(Host714ProtocolTests PRIVATE /W4 /WX /wd4100 /wd4200)
else()
    target_compile_options(Host714ProtocolTests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

if(HOST714_BUILD_TRANSPORT)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL REQUIRED)
    find_path(OPUS_INCLUDE_DIR NAMES opus/opus_multistream.h REQUIRED)
    # Use the production host's static library. Pass OPUS_LIBRARY explicitly
    # from its CMakeCache to pin the exact same archive in a release build.
    set(_host714_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a" ".lib")
    find_library(OPUS_LIBRARY NAMES opus libopus REQUIRED)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_host714_library_suffixes}")
    file(SHA256 "${OPUS_LIBRARY}" HOST714_OPUS_SHA256)
    message(STATUS "Host714 Opus archive: ${OPUS_LIBRARY} SHA256=${HOST714_OPUS_SHA256}")
    add_executable(Host714EncoderTests encoder_tests.cpp
        "${HOST_ROOT}/src/audio_encoder.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/support/audio_source.cpp"
        "${HOST_ROOT}/src/audio_protocol.cpp" "${HOST_ROOT}/src/audio_policy.cpp")
    target_include_directories(Host714EncoderTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support" "${HOST_ROOT}" "${OPUS_INCLUDE_DIR}")
    target_link_libraries(Host714EncoderTests PRIVATE "${OPUS_LIBRARY}")
    add_test(NAME host714_encoder_source COMMAND Host714EncoderTests)
    # The host and client each contain a different pinned nanors tree with global
    # C symbol names. Keep the actual Common receiver in its normal shared library
    # so it uses its own RS/crypto implementation and cannot interpose host symbols.
    set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
    set(BUILD_AUDIO_REGRESSION_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_ABI_REGRESSION_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${COMMON_SOURCE_DIR}" "common-receiver")
    if(WIN32)
        # Export only the replay interface. Auto-export would expose the DLL's
        # private static OpenSSL/RS symbols and collide with the real host code.
        set_target_properties(moonlight-common-c PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
        target_sources(moonlight-common-c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/receiver_exports.def")
    endif()
    target_compile_definitions(moonlight-common-c PRIVATE LI_AUDIO_TESTING)
    target_link_libraries(moonlight-common-c PRIVATE OpenSSL::Crypto)
    target_sources(moonlight-common-c PRIVATE "${COMMON_SOURCE_DIR}/tests/test_audio_receive.c")
    if(WIN32)
        target_compile_definitions(moonlight-common-c PRIVATE HAS_QOS_FLOWID=1 HAS_PQOS_FLOWID=1)
        # The pinned static OpenSSL's Windows RNG calls CNG. This dependency
        # must be explicit for MinGW too (not only MSVC imported targets).
        target_link_libraries(moonlight-common-c PRIVATE bcrypt)
    endif()
    if(MSVC)
        target_compile_options(moonlight-common-c PRIVATE /wd5287)
    endif()

    add_executable(Host714TransportTests transport_tests.cpp
        "${HOST_ROOT}/src/audio_encoder.cpp" "${HOST_ROOT}/src/audio_transport.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/support/audio_source.cpp"
        "${HOST_ROOT}/src/audio_protocol.cpp" "${HOST_ROOT}/src/audio_policy.cpp"
        "${HOST_ROOT}/src/crypto.cpp" "${HOST_ROOT}/src/rswrapper.c")
    target_include_directories(Host714TransportTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support" "${CMAKE_CURRENT_SOURCE_DIR}/support"
        "${HOST_ROOT}" "${HOST_ROOT}/third-party/nanors" "${HOST_ROOT}/third-party/nanors/deps/obl"
        "${COMMON_SOURCE_DIR}/src" "${COMMON_SOURCE_DIR}/tests" "${OPUS_INCLUDE_DIR}" "${OPUS_INCLUDE_DIR}/opus")
    target_link_libraries(Host714TransportTests PRIVATE moonlight-common-c OpenSSL::Crypto "${OPUS_LIBRARY}")
    target_compile_definitions(Host714TransportTests PRIVATE NOMINMAX)
    add_test(NAME host714_packetizer_failure_cleanup COMMAND Host714TransportTests --constructor-failure)
    if(MSVC)
        target_compile_options(Host714TransportTests PRIVATE /W4 /wd4200)
    else()
        target_compile_options(Host714TransportTests PRIVATE -Wall -Wextra -Wno-unused-parameter)
        if(MINGW)
            # Runtime artifacts are copied alongside the harness; no dependency
            # on the developer's MSYS2 PATH when it is run from the test package.
            target_link_options(Host714TransportTests PRIVATE -static-libgcc -static-libstdc++)
            target_link_options(Host714EncoderTests PRIVATE -static-libgcc -static-libstdc++)
            target_link_options(Host714ProtocolTests PRIVATE -static-libgcc -static-libstdc++)
            target_link_options(Host714DeliveryTests PRIVATE -static-libgcc -static-libstdc++)
            target_link_options(Host714SourceLifecycleTests PRIVATE -static-libgcc -static-libstdc++)
        endif()
    endif()
    add_test(NAME host714_production_transport COMMAND Host714TransportTests)
    add_test(NAME vibe_usb_confirmed_loss_timeline COMMAND Host714TransportTests --usb-loss)
    add_executable(Host714IngressTests ingress_tests.cpp
        "${HOST_ROOT}/src/audio_encoder.cpp" "${HOST_ROOT}/src/audio_transport.cpp"
        "${HOST_ROOT}/src/audio_protocol.cpp" "${HOST_ROOT}/src/audio_policy.cpp"
        "${HOST_ROOT}/src/crypto.cpp" "${HOST_ROOT}/src/rswrapper.c")
    target_include_directories(Host714IngressTests PRIVATE "${HOST_ROOT}" "${OPUS_INCLUDE_DIR}"
        "${HOST_ROOT}/third-party/nanors" "${HOST_ROOT}/third-party/nanors/deps/obl")
    target_link_libraries(Host714IngressTests PRIVATE OpenSSL::Crypto "${OPUS_LIBRARY}" Threads::Threads)
    target_compile_definitions(Host714IngressTests PRIVATE NOMINMAX)
    if(MINGW)
        target_link_options(Host714IngressTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME host714_preallocated_ingress COMMAND Host714IngressTests)
    if(WIN32)
        target_link_libraries(Host714TransportTests PRIVATE bcrypt)
        target_link_libraries(Host714IngressTests PRIVATE bcrypt)
    endif()
    set_tests_properties(host714_preallocated_ingress PROPERTIES TIMEOUT 30)
    set_tests_properties(host714_encoder_source host714_production_transport PROPERTIES TIMEOUT 120)
    add_custom_command(TARGET Host714TransportTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:moonlight-common-c>" "$<TARGET_FILE_DIR:Host714TransportTests>"
        VERBATIM)

    add_executable(AtmosDeviceTests atmos_device_tests.cpp
        "${HOST_ROOT}/src/audio_atmos_device.cpp" "${HOST_ROOT}/src/audio_encoder.cpp"
        "${HOST_ROOT}/src/audio_protocol.cpp" "${HOST_ROOT}/src/audio_policy.cpp")
    target_include_directories(AtmosDeviceTests PRIVATE "${HOST_ROOT}" "${OPUS_INCLUDE_DIR}")
    target_link_libraries(AtmosDeviceTests PRIVATE "${OPUS_LIBRARY}" Threads::Threads)
    add_test(NAME atmos_product_format_lease COMMAND AtmosDeviceTests)
    if(MINGW)
        target_link_options(AtmosDeviceTests PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(WIN32)
        add_executable(VibeUsbMmcssTests usb_mmcss_tests.cpp)
        target_include_directories(VibeUsbMmcssTests PRIVATE "${HOST_ROOT}")
        target_link_libraries(VibeUsbMmcssTests PRIVATE avrt Threads::Threads)
        if(MINGW)
            target_link_options(VibeUsbMmcssTests PRIVATE -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME vibe_usb_mmcss COMMAND VibeUsbMmcssTests)
        add_executable(VibeUsbPreOpusSignatureTests usb_preopus_signature_tests.cpp)
        target_include_directories(VibeUsbPreOpusSignatureTests PRIVATE "${HOST_ROOT}")
        if(MINGW)
            target_link_options(VibeUsbPreOpusSignatureTests PRIVATE -static-libgcc -static-libstdc++)
        endif()
        add_test(NAME vibe_usb_preopus_signatures COMMAND VibeUsbPreOpusSignatureTests)
    endif()
endif()
