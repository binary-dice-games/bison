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

# C API tests — link bison_c shared library instead of bison static library
add_executable(bison_c_test bison_c_tests.cpp)
target_include_directories(bison_c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bison_c_test bison_c bison gtest gmock gtest_main)
add_custom_command(TARGET bison_c_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:bison_c>
            $<TARGET_FILE_DIR:bison_c_test>
)
gtest_discover_tests(bison_c_test
    WORKING_DIRECTORY ${PROJECT_DIR}
    PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_DIR}"
)
set_target_properties(bison_c_test PROPERTIES FOLDER tests)
