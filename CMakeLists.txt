# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.11...3.27)

project(
  Bison
  VERSION 1.0
  LANGUAGES CXX)

option(PACKAGE_TESTS "Build the tests" ON)
  
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(BISON_PUBLIC_HEADERS
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison_common.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison_sync.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison_serialization.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/core/bison_object.hpp"
)

set(BISON_PRIVATE_SOURCES
  src/core/bison.cpp
)

add_library(asio_headers INTERFACE)
target_include_directories(
  asio_headers
  INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/extern/asio/include
)
target_compile_definitions(
  asio_headers
  INTERFACE
    ASIO_STANDALONE
    ASIO_DISABLE_CO_AWAIT
    ASIO_DISABLE_STD_COROUTINE
)

if(WIN32)
  target_compile_definitions(
    asio_headers
    INTERFACE
      _WIN32_WINNT=0x0601
      WIN32_LEAN_AND_MEAN
      NOMINMAX
  )
endif()

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
target_link_libraries(bison PRIVATE asio_headers)
# Required when this static library is linked into a shared library.
set_property(TARGET bison PROPERTY POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(bison PRIVATE /FS)
endif()

# ─── RMI framework sources ────────────────────────────────────────────────────
target_sources(bison PRIVATE
  src/rmi/shared/ids.cpp  src/rmi/shared/envelope.cpp
  src/rmi/transport/memory_transport.cpp
  src/rmi/transport/socket_transport.cpp
  src/rmi/transport/stdio_transport.cpp
  src/rmi/client/client.cpp
  src/rmi/client/proxy.cpp
  src/rmi/server/server.cpp
)

if(WIN32)
  target_link_libraries(bison PRIVATE Ws2_32)
endif()

target_include_directories(bison PRIVATE src/rmi)

# ─── Shared library (pure-C ABI for core) ──────────────────────────────────
add_library(bison_core_c SHARED
  src/core/bison_c.h
  src/core/bison_c.cpp
)

# bison_core_c is a superset of bison; it needs the same dependencies.
target_compile_definitions(bison_core_c PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_core_c
    PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src/core
        ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(bison_core_c PRIVATE bison nlohmann_json::nlohmann_json yaml)
if(MSVC)
  target_compile_options(bison_core_c PRIVATE /FS)
endif()

# ─── Shared library (pure-C ABI for RMI) ───────────────────────────────────
add_library(bison_rmi_c SHARED
  src/rmi/rmi_c.h
  src/rmi/rmi_c.cpp
)

# bison_rmi_c depends on bison_core_c and bison (C++ static lib)
target_compile_definitions(bison_rmi_c PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_rmi_c
    PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src/core
        ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE
  src/rmi
)
target_link_libraries(bison_rmi_c PRIVATE bison bison_core_c)
if(MSVC)
  target_compile_options(bison_rmi_c PRIVATE /FS)
endif()

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
