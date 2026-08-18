# Verify that the MSI Binary custom action is standalone.  This runs after the
# DLL is linked and before package_msi can complete; a missing or unexpected
# imported DLL would make MSI load an untrusted or unavailable runtime.

if(NOT DEFINED TERMINAL_ISOLATION_CA_DLL OR NOT EXISTS "${TERMINAL_ISOLATION_CA_DLL}")
    message(FATAL_ERROR "TerminalIsolationCA import gate did not receive a built DLL")
endif()
if(NOT DEFINED TERMINAL_ISOLATION_IMPORT_TOOL OR NOT EXISTS "${TERMINAL_ISOLATION_IMPORT_TOOL}")
    message(FATAL_ERROR "TerminalIsolationCA import gate has no PE inspection tool")
endif()

if(TERMINAL_ISOLATION_IMPORT_MODE STREQUAL "objdump")
    execute_process(
        COMMAND "${TERMINAL_ISOLATION_IMPORT_TOOL}" -p "${TERMINAL_ISOLATION_CA_DLL}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
else()
    execute_process(
        COMMAND "${TERMINAL_ISOLATION_IMPORT_TOOL}" /DEPENDENTS "${TERMINAL_ISOLATION_CA_DLL}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
endif()
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "TerminalIsolationCA import inspection failed: ${_error}")
endif()

if(TERMINAL_ISOLATION_IMPORT_MODE STREQUAL "objdump")
    string(REGEX MATCHALL "DLL Name:[ \t]*[^ \t\r\n]+" _matches "${_output}")
else()
    # dumpbin /DEPENDENTS prints one indented DLL name per dependency.
    string(REGEX MATCHALL "[ \t][ \t][ \t][ \t][A-Za-z0-9_.-]+\\.dll" _matches "${_output}")
endif()
if(NOT _matches)
    message(FATAL_ERROR "TerminalIsolationCA import inspection found no PE imports")
endif()

foreach(_match IN LISTS _matches)
    if(TERMINAL_ISOLATION_IMPORT_MODE STREQUAL "objdump")
        string(REGEX REPLACE "^[Dd][Ll][Ll] [Nn]ame:[ \t]*" "" _name "${_match}")
    else()
        string(REGEX REPLACE "^[ \t]+" "" _name "${_match}")
    endif()
    string(TOLOWER "${_name}" _name)
    if(NOT _name MATCHES "^(msi|advapi32|bcrypt|kernel32|kernelbase|ntdll|msvcrt|ucrtbase)\\.dll$" AND
       NOT _name MATCHES "^(api-ms-win|ext-ms-win)-[^ ]+\\.dll$")
        message(FATAL_ERROR "TerminalIsolationCA imports unexpected DLL: ${_name}")
    endif()
endforeach()
