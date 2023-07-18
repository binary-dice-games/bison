# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Bison
  VERSION 1.0
  LANGUAGES CXX)

option(PACKAGE_TESTS "Build the tests" ON)
  
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(BISON_PUBLIC_HEADERS
  	"${CMAKE_CURRENT_LIST_DIR}/src/bison.hpp"
)

set(BISON_PRIVATE_SOURCES
	src/bison.cpp
)

include_directories(. include)

add_subdirectory("extern/json")

add_library(bison STATIC
  ${BISON_PUBLIC_HEADERS}
  ${BISON_PRIVATE_SOURCES}
)

target_include_directories(bison
    PRIVATE
        src
    PUBLIC
        include
)

target_link_libraries(bison PRIVATE nlohmann_json::nlohmann_json)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
