set(BISON_EXAMPLES_PRIVATE_SOURCES
	main.cpp
)

add_executable(bison_examples ${BISON_EXAMPLES_PRIVATE_SOURCES})

target_link_libraries(bison_examples PRIVATE bison)