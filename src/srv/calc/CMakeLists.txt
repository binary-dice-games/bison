add_executable(calc-server
  calc_server.cpp
  main.cpp
)

target_include_directories(calc-server PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/rmi)
target_link_libraries(calc-server PRIVATE bison gflags)
set_target_properties(calc-server PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
