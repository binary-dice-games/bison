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

# Perfetto track-event wire-format encoder tests.
package_add_test(bison_perfetto_test bison_perfetto_tests.cpp)

# RMI framework tests (header-only, no extra link deps beyond bison)
package_add_test(rmi_test rmi_tests.cpp)

# RMI Perfetto profiler tests (recorder unit tests + server/client integration).
package_add_test(rmi_profiler_test rmi_profiler_tests.cpp)

# Bridge multiplexer tests
package_add_test(bridge_test bridge_tests.cpp)

# App scaffold tests (standalone_app, bridge_app, server_app, client_app) —
# these define gflags flags directly (see the .cpp files for why), so they
# need gflags linked/included.
package_add_test(standalone_app_test standalone_app_tests.cpp)
target_link_libraries(standalone_app_test gflags)
package_add_test(bridge_app_test bridge_app_tests.cpp)
target_link_libraries(bridge_app_test gflags)
package_add_test(server_app_test server_app_tests.cpp)
target_link_libraries(server_app_test gflags)
package_add_test(client_app_test client_app_tests.cpp)
target_link_libraries(client_app_test gflags)

# line_edit_state editing-rules tests (terminal-agnostic; no tty needed)
package_add_test(line_editor_test line_editor_tests.cpp)

# term (OSC-99 chunked) transport tests — cross-platform
package_add_test(term_transport_test term_transport_tests.cpp)

# TLS socket transport tests — cross-platform
package_add_test(tls_socket_transport_test tls_socket_transport_tests.cpp)

# terminal tests (Linux and MSYS2; native-Windows ConPTY path is untested here)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(terminal_posix_test terminal_posix_tests.cpp)
endif()

# scoped_terminal_config tests (Linux and MSYS2; native-Windows console-mode
# path is untested here, same as terminal_posix_test)
if(NOT WIN32 OR MSYS OR CYGWIN)
    package_add_test(scoped_terminal_config_test scoped_terminal_config_tests.cpp)
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
