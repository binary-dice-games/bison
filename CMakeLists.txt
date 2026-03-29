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
# Allow legacy subprojects (libyaml currently declares cmake_minimum_required 3.0).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("extern/yaml")

# ─── Static library (C++ API) ────────────────────────────────────────────────
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

target_link_libraries(bison PRIVATE nlohmann_json::nlohmann_json yaml)
# Required when this static library is linked into a shared library.
set_property(TARGET bison PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─── Shared library (pure-C ABI) ─────────────────────────────────────────────
add_library(bison_c SHARED
  src/bison_c.h
  src/bison_c.cpp
)

# bison_c is a superset of bison; it needs the same dependencies.
target_compile_definitions(bison_c PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_c
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(bison_c PRIVATE bison nlohmann_json::nlohmann_json yaml)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
