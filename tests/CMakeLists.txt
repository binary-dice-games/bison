add_subdirectory("${PROJECT_SOURCE_DIR}/extern/googletest" "extern/googletest")

set_target_properties(gtest PROPERTIES FOLDER extern)
set_target_properties(gtest_main PROPERTIES FOLDER extern)
set_target_properties(gmock PROPERTIES FOLDER extern)
set_target_properties(gmock_main PROPERTIES FOLDER extern)

macro(package_add_test TESTNAME)
    add_executable(${TESTNAME} ${ARGN})
    target_link_libraries(${TESTNAME} bison gtest gmock gtest_main)
    target_include_directories(${TESTNAME} PRIVATE ${PROJECT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${TESTNAME} PRIVATE /FS)
    endif()
    gtest_discover_tests(${TESTNAME}
        WORKING_DIRECTORY ${PROJECT_DIR}
        PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
    )
    set_target_properties(${TESTNAME} PROPERTIES FOLDER tests)
endmacro()

package_add_test(bison_test bison_tests.cpp)

# RMI framework tests (header-only, no extra link deps beyond bison)
package_add_test(rmi_test rmi_tests.cpp)

# PTY transport and app scaffold tests (Linux only; links util for forkpty)
# app_test_flags.cpp provides DEFINE_* for gflags referenced by server_app /
# client_app, which are pulled in transitively via pty_server_app.
package_add_test(pty_test pty_tests.cpp app_test_flags.cpp)
target_link_libraries(pty_test gflags)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pty_test util)
endif()

# PTY C API tests — link bison_abi shared library
add_executable(pty_c_test pty_c_tests.cpp)
target_include_directories(pty_c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(pty_c_test bison_abi bison gtest gmock gtest_main)
if(MSVC)
    target_compile_options(pty_c_test PRIVATE /FS)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pty_c_test util)
endif()
gtest_discover_tests(pty_c_test
    WORKING_DIRECTORY ${PROJECT_DIR}
    PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
)
set_target_properties(pty_c_test PROPERTIES FOLDER tests)

# C API tests — link bison_abi shared library instead of bison static library
add_executable(bison_c_test bison_c_tests.cpp)
target_include_directories(bison_c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bison_c_test bison_abi bison gtest gmock gtest_main)
if(MSVC)
    target_compile_options(bison_c_test PRIVATE /FS)
endif()
if(WIN32)
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
if(MSVC)
    target_compile_options(rmi_c_test PRIVATE /FS)
endif()
if(WIN32)
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
