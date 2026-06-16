set(BISON_EXAMPLES_PRIVATE_SOURCES
	bison_example.cpp
)

set(BISON_ABI_EXAMPLES_PRIVATE_SOURCES
	bison_abi_example.cpp
)

set(BISON_PERFORMANCE_PRIVATE_SOURCES
	performance.cpp
)

set(BISON_RMI_SERVER_EXAMPLE_PRIVATE_SOURCES
	rmi_server_example.cpp
)

set(BISON_RMI_CLIENT_EXAMPLE_PRIVATE_SOURCES
	rmi_client_example.cpp
)

set(BISON_RMI_STDIO_SERVER_EXAMPLE_PRIVATE_SOURCES
	rmi_stdio_server_example.cpp
)

set(BISON_RMI_STDIO_CLIENT_EXAMPLE_PRIVATE_SOURCES
	rmi_stdio_client_example.cpp
)

set(BISON_PTY_SERVER_EXAMPLE_PRIVATE_SOURCES
	pty_server_example.cpp
)

set(BISON_RMI_STANDALONE_EXAMPLE_PRIVATE_SOURCES
	rmi_standalone_example.cpp
)

set(BISON_RMI_SERVER_ABI_EXAMPLE_PRIVATE_SOURCES
	rmi_server_abi_example.cpp
)

set(BISON_RMI_CLIENT_ABI_EXAMPLE_PRIVATE_SOURCES
	rmi_client_abi_example.cpp
)

set(BISON_RMI_STANDALONE_ABI_EXAMPLE_PRIVATE_SOURCES
	rmi_standalone_abi_example.cpp
)

set(BISON_PTY_CLIENT_EXAMPLE_PRIVATE_SOURCES
	pty_client_example.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})
add_executable(bison_abi_examples ${BISON_ABI_EXAMPLES_PRIVATE_SOURCES})
add_executable(bison_performance ${BISON_PERFORMANCE_PRIVATE_SOURCES})
add_executable(rmi_server_example ${BISON_RMI_SERVER_EXAMPLE_PRIVATE_SOURCES})
add_executable(rmi_client_example ${BISON_RMI_CLIENT_EXAMPLE_PRIVATE_SOURCES})
add_executable(
	rmi_stdio_server_example
	${BISON_RMI_STDIO_SERVER_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_stdio_client_example
	${BISON_RMI_STDIO_CLIENT_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_standalone_example
	${BISON_RMI_STANDALONE_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_server_abi_example
	${BISON_RMI_SERVER_ABI_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_client_abi_example
	${BISON_RMI_CLIENT_ABI_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_standalone_abi_example
	${BISON_RMI_STANDALONE_ABI_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(pty_server_example ${BISON_PTY_SERVER_EXAMPLE_PRIVATE_SOURCES})
add_executable(pty_client_example ${BISON_PTY_CLIENT_EXAMPLE_PRIVATE_SOURCES})

target_include_directories(bison_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_abi_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_performance PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_client_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_stdio_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_stdio_client_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_standalone_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_server_abi_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_client_abi_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_standalone_abi_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(pty_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(pty_client_example PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(bison_examples PRIVATE bison)
target_link_libraries(bison_abi_examples PRIVATE bison_abi)
target_link_libraries(bison_performance PRIVATE bison nlohmann_json::nlohmann_json)
target_link_libraries(rmi_server_example PRIVATE bison)
target_link_libraries(rmi_client_example PRIVATE bison)
target_link_libraries(rmi_stdio_server_example PRIVATE bison)
target_link_libraries(rmi_stdio_client_example PRIVATE bison)
target_link_libraries(rmi_standalone_example PRIVATE bison)
target_link_libraries(rmi_server_abi_example PRIVATE bison_abi)
target_link_libraries(rmi_client_abi_example PRIVATE bison_abi)
target_link_libraries(rmi_standalone_abi_example PRIVATE bison_abi)
target_link_libraries(pty_server_example PRIVATE bison)
target_link_libraries(pty_client_example PRIVATE bison)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(rmi_stdio_client_example PRIVATE util)
	target_link_libraries(pty_client_example PRIVATE util)
endif()

# On Windows the DLL must be co-located with the executable.  Copy bison_abi
# to each ABI example's output directory after every build so the examples
# can be run directly from their build folder.
if(WIN32)
	set(BISON_ABI_EXAMPLE_TARGETS
		bison_abi_examples
		rmi_server_abi_example
		rmi_client_abi_example
		rmi_standalone_abi_example
	)
	foreach(_target ${BISON_ABI_EXAMPLE_TARGETS})
		add_custom_command(TARGET ${_target} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				$<TARGET_FILE:bison_abi>
				$<TARGET_FILE_DIR:${_target}>
			COMMENT "Copying bison_abi.dll -> $<TARGET_FILE_DIR:${_target}>"
		)
	endforeach()
endif()