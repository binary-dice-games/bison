set(BISON_EXAMPLES_PRIVATE_SOURCES
	main.cpp
)

set(BISON_PERFORMANCE_PRIVATE_SOURCES
	performance.cpp
)

set(BISON_RMI_EXAMPLE_PRIVATE_SOURCES
	rmi_example.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})
add_executable(bison_performance ${BISON_PERFORMANCE_PRIVATE_SOURCES})
add_executable(rmi_example ${BISON_RMI_EXAMPLE_PRIVATE_SOURCES})

target_include_directories(bison_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_performance PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(rmi_example PRIVATE ${PROJECT_SOURCE_DIR})

target_link_libraries(bison_examples PRIVATE bison)
target_link_libraries(bison_performance PRIVATE bison nlohmann_json::nlohmann_json)
target_link_libraries(rmi_example PRIVATE bison)