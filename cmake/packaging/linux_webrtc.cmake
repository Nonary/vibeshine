# Preserve SONAME links and their real payload, including versioned SDKs.
if(SUNSHINE_ENABLE_WEBRTC AND WEBRTC_RUNTIME_LIB)
    get_filename_component(_webrtc_real "${WEBRTC_RUNTIME_LIB}" REALPATH)
    get_filename_component(_webrtc_dir "${WEBRTC_RUNTIME_LIB}" DIRECTORY)
    file(GLOB _webrtc_candidates "${_webrtc_dir}/libwebrtc.so*")
    set(_webrtc_runtime_files "${WEBRTC_RUNTIME_LIB}")
    foreach(_candidate IN LISTS _webrtc_candidates)
        get_filename_component(_candidate_real "${_candidate}" REALPATH)
        if(_candidate_real STREQUAL _webrtc_real)
            list(APPEND _webrtc_runtime_files "${_candidate}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _webrtc_runtime_files)
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        set(_webrtc_destination "${CMAKE_INSTALL_LIBDIR}")
    else()
        set(_webrtc_destination "\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
    endif()
    foreach(_library IN LISTS _webrtc_runtime_files)
        install(CODE "file(INSTALL \"${_library}\"
            DESTINATION \"${_webrtc_destination}\"
            TYPE SHARED_LIBRARY FOLLOW_SYMLINK_CHAIN)")
    endforeach()
    # GNUInstallDirs permits lib64, multiarch and absolute install directories.
    file(RELATIVE_PATH _webrtc_lib_relative
        "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    set_property(TARGET sunshine APPEND PROPERTY INSTALL_RPATH "$ORIGIN/${_webrtc_lib_relative}")
    if(VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR)
        get_filename_component(_webrtc_host_dir "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}"
            ABSOLUTE BASE_DIR "${CMAKE_INSTALL_PREFIX}")
        file(RELATIVE_PATH _webrtc_host_relative "${_webrtc_host_dir}" "${CMAKE_INSTALL_FULL_LIBDIR}")
        # The same bytes are installed at both executable locations. The
        # absolute path also serves capability-bearing secure-loader execution.
        set_property(TARGET sunshine APPEND PROPERTY INSTALL_RPATH
            "$ORIGIN/${_webrtc_host_relative}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    endif()
    unset(_webrtc_lib_relative)
endif()

# Copy the already RPATH-adjusted installed binary, not the build-tree binary.
# Keep the host a distinct inode but byte-identical to the public executable.
function(vibeshine_install_webrtc_host)
    if(IS_ABSOLUTE "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
        set(_host_destination "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
    else()
        set(_host_destination "\${CMAKE_INSTALL_PREFIX}/${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}")
    endif()
    if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}")
        set(_public_directory "${CMAKE_INSTALL_BINDIR}")
    else()
        set(_public_directory "\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")
    endif()
    install(CODE "file(INSTALL \"\$ENV{DESTDIR}${_public_directory}/$<TARGET_FILE_NAME:sunshine>\"
        DESTINATION \"${_host_destination}\" TYPE PROGRAM RENAME vibeshine-host
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)")
endfunction()
