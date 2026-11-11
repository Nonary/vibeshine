# The native C API needs no SDK headers or Python at runtime. Retain the pinned
# wheel's layout/SONAMEs, licenses and provenance in the immutable library tree.
option(SUNSHINE_ENABLE_NV_TRUEHDR "Enable native NVIDIA TrueHDR on Linux" ON)
if(SUNSHINE_ENABLE_NV_TRUEHDR AND CUDA_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(SUNSHINE_TRUEHDR_LINUX_ROOT "${CMAKE_BINARY_DIR}/truehdr-runtime" CACHE PATH "Extracted pinned NVIDIA VFX runtime")
    if(NOT EXISTS "${SUNSHINE_TRUEHDR_LINUX_ROOT}/runtime-provenance.txt")
        execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/linux_truehdr/fetch_runtime.py"
                        "${SUNSHINE_TRUEHDR_LINUX_ROOT}" COMMAND_ERROR_IS_FATAL ANY)
    endif()
    file(READ "${SUNSHINE_TRUEHDR_LINUX_ROOT}/runtime-provenance.txt" _truehdr_provenance)
    if(NOT _truehdr_provenance MATCHES "sha256=0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8")
        message(FATAL_ERROR "NVIDIA TrueHDR runtime provenance does not match the pinned wheel")
    endif()
    # Only the TrueHDR dependency closure is needed. The wheel also contains
    # NPP libraries for other effects; those add hundreds of MB on disk.
    set(_truehdr_libraries libnppc.so.12 libnppial.so.12 libnppicc.so.12
            libnppidei.so.12 libnppig.so.12 libcudnn.so.9 libNVCVImage.so libnvngxruntime.so
            libVideoFXLocal.so libVideoFX.so libnvVFXTrueHDR.so libnvidia-ngx-truehdr.so.1.2.3)
    foreach(_truehdr_library IN LISTS _truehdr_libraries)
        if(NOT EXISTS "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvvfx/libs/${_truehdr_library}")
            message(FATAL_ERROR "Pinned TrueHDR runtime is missing ${_truehdr_library}")
        endif()
    endforeach()
    set(_truehdr_cudart "${SUNSHINE_TRUEHDR_LINUX_ROOT}/nvidia_vfx.libs/libcudart-256e6409.so.12.9.79")
    if(NOT EXISTS "${_truehdr_cudart}")
        message(FATAL_ERROR "Pinned TrueHDR runtime is missing its CUDA runtime")
    endif()
    add_library(linux-truehdr STATIC "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr_gl.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/linux/truehdr_peak.cu")
    target_compile_features(linux-truehdr PUBLIC cxx_std_20)
    target_link_libraries(linux-truehdr PRIVATE glad ${CMAKE_DL_LIBS})
    list(APPEND PLATFORM_LIBRARIES linux-truehdr)
    list(APPEND SUNSHINE_DEFINITIONS SUNSHINE_ENABLE_NV_TRUEHDR=1
        SUNSHINE_TRUEHDR_HELPER="${CMAKE_INSTALL_FULL_LIBEXECDIR}/vibeshine/vibeshine-truehdr-runtime")
    # Install the small verified downloader; the NVIDIA runtime is fetched into
    # service-owned state only when RTX HDR is enabled.
    install(PROGRAMS "${CMAKE_SOURCE_DIR}/tools/linux_truehdr/fetch_runtime.py"
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/vibeshine"
        RENAME "vibeshine-truehdr-runtime")
endif()
