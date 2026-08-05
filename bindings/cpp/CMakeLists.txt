# bindings/cpp/CMakeLists.txt
#
# The C++ binding itself (bindings/cpp/include/bison/*.hpp) is header-only:
# a third-party project consuming it needs nothing from this file, just
# `#include "bison/bison.hpp"` plus the precompiled bison_abi shared library
# and its bison_c.h/rmi_c.h headers (see docs/bindings.md). This file only
# builds this repository's own examples and tests for that header against
# the bison_abi target already defined in the root CMakeLists.txt.

add_library(bison_cpp_binding INTERFACE)
target_include_directories(bison_cpp_binding INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(bison_cpp_binding INTERFACE bison_abi)

# ─── Examples ─────────────────────────────────────────────────────────────

add_executable(bison_cpp_example examples/bison_example.cpp)
target_link_libraries(bison_cpp_example PRIVATE bison_cpp_binding)

add_executable(rmi_cpp_standalone_example examples/rmi_standalone_example.cpp)
target_link_libraries(rmi_cpp_standalone_example PRIVATE bison_cpp_binding)

add_executable(rmi_cpp_server_example examples/rmi_server_example.cpp)
target_link_libraries(rmi_cpp_server_example PRIVATE bison_cpp_binding)

add_executable(rmi_cpp_client_example examples/rmi_client_example.cpp)
target_link_libraries(rmi_cpp_client_example PRIVATE bison_cpp_binding)

# bison_abi is a shared library; on Windows/MSYS2 the DLL must sit next to
# the executable that loads it (same pattern as examples/CMakeLists.txt's
# *_abi_* targets).
foreach(_cpp_example_target bison_cpp_example rmi_cpp_standalone_example rmi_cpp_server_example rmi_cpp_client_example)
    add_custom_command(TARGET ${_cpp_example_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:bison_abi>
            $<TARGET_FILE_DIR:${_cpp_example_target}>/$<TARGET_FILE_NAME:bison_abi>
        COMMENT "Copying bison_abi DLL next to ${_cpp_example_target}"
    )
endforeach()

# ─── Tests ────────────────────────────────────────────────────────────────

if(PACKAGE_TESTS)
    add_executable(bison_cpp_test tests/dynamic_tests.cpp)
    target_link_libraries(bison_cpp_test PRIVATE bison_cpp_binding gtest gmock gtest_main)
    if(WIN32 OR MSYS OR CYGWIN)
        add_custom_command(TARGET bison_cpp_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:bison_abi>
                    $<TARGET_FILE_DIR:bison_cpp_test>)
    endif()
    gtest_discover_tests(bison_cpp_test
        TEST_PREFIX "cpp_binding."
        WORKING_DIRECTORY ${PROJECT_DIR}
        PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
    )
    set_target_properties(bison_cpp_test PROPERTIES FOLDER tests)

    add_executable(rmi_cpp_test tests/rmi_tests.cpp)
    target_link_libraries(rmi_cpp_test PRIVATE bison_cpp_binding gtest gmock gtest_main)
    if(WIN32 OR MSYS OR CYGWIN)
        add_custom_command(TARGET rmi_cpp_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:bison_abi>
                    $<TARGET_FILE_DIR:rmi_cpp_test>)
    endif()
    gtest_discover_tests(rmi_cpp_test
        TEST_PREFIX "cpp_binding."
        WORKING_DIRECTORY ${PROJECT_DIR}
        PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
    )
    set_target_properties(rmi_cpp_test PROPERTIES FOLDER tests)
endif()
