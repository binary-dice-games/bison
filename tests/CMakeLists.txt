add_subdirectory("${PROJECT_SOURCE_DIR}/extern/googletest" "extern/googletest")

set_target_properties(gtest PROPERTIES FOLDER extern)
set_target_properties(gtest_main PROPERTIES FOLDER extern)
set_target_properties(gmock PROPERTIES FOLDER extern)
set_target_properties(gmock_main PROPERTIES FOLDER extern)

macro(package_add_test TESTNAME)
    add_executable(${TESTNAME} ${ARGN})
    target_link_libraries(${TESTNAME} bison gtest gmock gtest_main)
    target_include_directories(${TESTNAME} PRIVATE ${PROJECT_SOURCE_DIR})
    gtest_discover_tests(${TESTNAME}
        WORKING_DIRECTORY ${PROJECT_DIR}
        PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
    )
    set_target_properties(${TESTNAME} PROPERTIES FOLDER tests)
endmacro()

package_add_test(bison_test bison_tests.cpp)

# RMI framework tests (header-only, no extra link deps beyond bison)
package_add_test(rmi_test rmi_tests.cpp)

# Bridge multiplexer tests
package_add_test(bridge_test bridge_tests.cpp)

# stdio (BISON: line-framed) transport tests — cross-platform
package_add_test(stdio_transport_test stdio_transport_tests.cpp)

# term (OSC-99 chunked) transport tests — cross-platform
package_add_test(term_transport_test term_transport_tests.cpp)

# pty_process tests (Linux and MSYS2 only; pty_process.cpp isn't built on native Windows)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(pty_process_test pty_process_tests.cpp)
endif()

# terminal tests (Linux and MSYS2; native-Windows ConPTY path is untested here)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(terminal_posix_test terminal_posix_tests.cpp)
endif()

# console_process tests (libuv uv_spawn; Linux and MSYS2 only, see CMakeLists.txt)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(console_process_test console_process_tests.cpp)
endif()

# raw_mode_guard tests (POSIX termios behavior; native-Windows console-mode path is untested here)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(raw_mode_guard_test raw_mode_guard_tests.cpp)
endif()

# crlf_output_guard tests
package_add_test(crlf_output_guard_test crlf_output_guard_tests.cpp)

# pty_write tests (uses POSIX pipe()/read() to exercise write_raw(); the
# Windows write_raw() implementation itself is untested here, see CMakeLists.txt)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(pty_write_test pty_write_tests.cpp)
endif()

# C API tests — link bison_abi shared library instead of bison static library
add_executable(bison_c_test bison_c_tests.cpp)
target_include_directories(bison_c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bison_c_test bison_abi bison gtest gmock gtest_main)
if(WIN32 OR MSYS OR CYGWIN)
    add_custom_command(TARGET bison_c_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bison_abi>
                $<TARGET_FILE_DIR:bison_c_test>
    )
endif()
gtest_discover_tests(bison_c_test
    WORKING_DIRECTORY ${PROJECT_DIR}
    PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
)
set_target_properties(bison_c_test PROPERTIES FOLDER tests)

# RMI C API tests — link bison_abi shared library instead of separate C ABI libraries
add_executable(rmi_c_test rmi_c_tests.cpp)
target_include_directories(rmi_c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(rmi_c_test bison_abi bison gtest gmock gtest_main)
if(WIN32 OR MSYS OR CYGWIN)
    add_custom_command(TARGET rmi_c_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:bison_abi>
                $<TARGET_FILE_DIR:rmi_c_test>
    )
endif()
gtest_discover_tests(rmi_c_test
    WORKING_DIRECTORY ${PROJECT_DIR}
    PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
)
set_target_properties(rmi_c_test PROPERTIES FOLDER tests)
