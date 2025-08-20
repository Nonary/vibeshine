# windows specific target definitions
set_target_properties(sunshine PROPERTIES LINK_SEARCH_START_STATIC 1)
set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll")
find_library(ZLIB ZLIB1)
list(APPEND SUNSHINE_EXTERNAL_LIBRARIES
        $<TARGET_OBJECTS:sunshine_rc_object>
        Windowsapp.lib
        Wtsapi32.lib)

# Copy Playnite plugin sources into build output (for packaging/installers)
add_custom_target(copy_playnite_plugin ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/plugins/playnite" "${CMAKE_BINARY_DIR}/plugins/playnite"
        COMMENT "Copying Playnite plugin sources")
add_dependencies(sunshine copy_playnite_plugin)

# Enable libdisplaydevice logging in the main Sunshine binary only
target_compile_definitions(sunshine PRIVATE SUNSHINE_USE_DISPLAYDEVICE_LOGGING)
