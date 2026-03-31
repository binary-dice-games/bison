set(BISON_EXAMPLES_PRIVATE_SOURCES
	bison_example.cpp
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

set(BISON_PTY_CLIENT_EXAMPLE_PRIVATE_SOURCES
	pty_client_example.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})
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
add_executable(pty_server_example ${BISON_PTY_SERVER_EXAMPLE_PRIVATE_SOURCES})
add_executable(pty_client_example ${BISON_PTY_CLIENT_EXAMPLE_PRIVATE_SOURCES})

target_include_directories(bison_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_performance PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_client_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_stdio_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_stdio_client_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(pty_server_example PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(pty_client_example PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(bison_examples PRIVATE bison)
target_link_libraries(bison_performance PRIVATE bison nlohmann_json::nlohmann_json)
target_link_libraries(rmi_server_example PRIVATE bison)
target_link_libraries(rmi_client_example PRIVATE bison)
target_link_libraries(rmi_stdio_server_example PRIVATE bison)
target_link_libraries(rmi_stdio_client_example PRIVATE bison)
target_link_libraries(pty_server_example PRIVATE bison)
target_link_libraries(pty_client_example PRIVATE bison)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_link_libraries(rmi_stdio_client_example PRIVATE util)
	target_link_libraries(pty_client_example PRIVATE util)
endif()