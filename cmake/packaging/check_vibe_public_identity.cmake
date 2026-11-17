# The upstream USB/IP transport can be shared by unrelated devices. Only the
# Vibe Speakers function is subject to this identity check.
if(CPACK_VIBE_PRIVATE_IDENTITY)
    message(FATAL_ERROR "The private 1209:7140 endpoint build cannot be publicly packaged while pidcodes/pidcodes#1384 is pending.")
endif()
file(GLOB_RECURSE package_files LIST_DIRECTORIES false "${CPACK_TEMPORARY_DIRECTORY}/*")
foreach(file IN LISTS package_files)
    get_filename_component(name "${file}" NAME)
    string(TOLOWER "${name}" name)
    if(name STREQUAL "vibeusbbridge.exe")
        message(FATAL_ERROR "Vibe Speakers still uses reserved in-house USB PIDs. This build of the endpoint cannot ship in the public installer or portable archive.")
    endif()
endforeach()
