add_executable(bison-cli main.cpp)
target_link_libraries(bison-cli PRIVATE bison nlohmann_json::nlohmann_json gflags)
target_include_directories(bison-cli PRIVATE ${PROJECT_SOURCE_DIR}/src/rmi)
set_target_properties(bison-cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
install(TARGETS bison-cli RUNTIME DESTINATION bin COMPONENT bison)
