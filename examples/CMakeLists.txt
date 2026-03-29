set(BISON_EXAMPLES_PRIVATE_SOURCES
	main.cpp
)

set(BISON_PERFORMANCE_PRIVATE_SOURCES
	performance.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})
add_executable(bison_performance ${BISON_PERFORMANCE_PRIVATE_SOURCES})

target_include_directories(bison_examples PRIVATE ${PROJECT_SOURCE_DIR})
target_include_directories(bison_performance PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(bison_examples PRIVATE bison)
target_link_libraries(bison_performance PRIVATE bison nlohmann_json::nlohmann_json)