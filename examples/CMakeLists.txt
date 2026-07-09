set(BISON_EXAMPLES_PRIVATE_SOURCES
	bison_example.cpp
)

set(BISON_ABI_EXAMPLES_PRIVATE_SOURCES
	bison_abi_example.cpp
)

set(BISON_RMI_ABI_SERVER_EXAMPLE_PRIVATE_SOURCES
	rmi_abi_server_example.cpp
)

set(BISON_RMI_ABI_CLIENT_EXAMPLE_PRIVATE_SOURCES
	rmi_abi_client_example.cpp
)

set(BISON_RMI_ABI_STANDALONE_EXAMPLE_PRIVATE_SOURCES
	rmi_abi_standalone_example.cpp
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

set(BISON_RMI_STANDALONE_EXAMPLE_PRIVATE_SOURCES
	rmi_standalone_example.cpp
)

set(BISON_RMI_BRIDGE_EXAMPLE_PRIVATE_SOURCES
	rmi_bridge_example.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})
add_executable(bison_abi_examples ${BISON_ABI_EXAMPLES_PRIVATE_SOURCES})
add_executable(
	rmi_abi_server_example
	${BISON_RMI_ABI_SERVER_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_abi_client_example
	${BISON_RMI_ABI_CLIENT_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_abi_standalone_example
	${BISON_RMI_ABI_STANDALONE_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(bison_performance ${BISON_PERFORMANCE_PRIVATE_SOURCES})
add_executable(rmi_server_example ${BISON_RMI_SERVER_EXAMPLE_PRIVATE_SOURCES})
add_executable(rmi_client_example ${BISON_RMI_CLIENT_EXAMPLE_PRIVATE_SOURCES})
add_executable(
	rmi_standalone_example
	${BISON_RMI_STANDALONE_EXAMPLE_PRIVATE_SOURCES}
)
add_executable(
	rmi_bridge_example
	${BISON_RMI_BRIDGE_EXAMPLE_PRIVATE_SOURCES}
)

target_include_directories(bison_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_abi_examples PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(rmi_abi_server_example PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(rmi_abi_client_example PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(rmi_abi_standalone_example PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(bison_performance PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_client_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_standalone_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_bridge_example PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(bison_examples PRIVATE bison)
target_link_libraries(bison_abi_examples PRIVATE bison_abi)
target_link_libraries(rmi_abi_server_example PRIVATE bison_abi)
target_link_libraries(rmi_abi_client_example PRIVATE bison_abi)
target_link_libraries(rmi_abi_standalone_example PRIVATE bison_abi)

foreach(_abi_target bison_abi_examples rmi_abi_server_example rmi_abi_client_example rmi_abi_standalone_example)
	add_custom_command(TARGET ${_abi_target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:bison_abi>
			$<TARGET_FILE_DIR:${_abi_target}>/$<TARGET_FILE_NAME:bison_abi>
		COMMENT "Copying bison_abi DLL next to ${_abi_target}"
	)
endforeach()

target_link_libraries(bison_performance PRIVATE bison nlohmann_json::nlohmann_json)
target_link_libraries(rmi_server_example PRIVATE bison gflags)
target_link_libraries(rmi_client_example PRIVATE bison gflags)
target_link_libraries(rmi_standalone_example PRIVATE bison gflags)
target_link_libraries(rmi_bridge_example PRIVATE bison gflags)
