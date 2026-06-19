add_executable(calc-server
  calc_server.cpp
  main.cpp
)

target_include_directories(calc-server PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/rmi)
target_link_libraries(calc-server PRIVATE bison)
