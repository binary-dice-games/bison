# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.10...3.27)

project(
  Bison
  VERSION 1.0
  LANGUAGES CXX)

option(PACKAGE_TESTS "Build the tests" ON)
  
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(BISON_PUBLIC_HEADERS
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison.hpp"
)

set(BISON_PRIVATE_SOURCES
  src/core/bison.cpp
)

include_directories(. include)

add_subdirectory("extern/json")
# Allow legacy subprojects (libyaml currently declares cmake_minimum_required 3.0).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("extern/yaml")
# libyaml is used in a shared library; it must be compiled with -fPIC.
set_property(TARGET yaml PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─── Static library (C++ API) ────────────────────────────────────────────────
add_library(bison STATIC
  ${BISON_PUBLIC_HEADERS}
  ${BISON_PRIVATE_SOURCES}
)

target_include_directories(bison
    PRIVATE
    src/core
    PUBLIC
        include
)

target_link_libraries(bison PRIVATE nlohmann_json::nlohmann_json yaml)
# Required when this static library is linked into a shared library.
set_property(TARGET bison PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─── RMI framework sources ────────────────────────────────────────────────────
target_sources(bison PRIVATE
  src/rmi/shared/ids.cpp  src/rmi/shared/envelope.cpp
  src/rmi/transport/memory_transport.cpp
  src/rmi/client/client.cpp
  src/rmi/server/server.cpp
)

if(WIN32)
  target_sources(bison PRIVATE
    src/rmi/transport/platform/windows/socket_transport_windows.cpp
    src/rmi/transport/platform/windows/pipe_transport_windows.cpp
  )
  target_link_libraries(bison PRIVATE Ws2_32)
endif()

target_include_directories(bison PRIVATE src/rmi)

# ─── Shared library (pure-C ABI) ─────────────────────────────────────────────
add_library(bison_c SHARED
  src/core/bison_c.h
  src/core/bison_c.cpp
)

# bison_c is a superset of bison; it needs the same dependencies.
target_compile_definitions(bison_c PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_c
    PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src/core
        ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(bison_c PRIVATE bison nlohmann_json::nlohmann_json yaml)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
