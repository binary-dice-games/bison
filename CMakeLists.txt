# https://cliutils.gitlab.io/modern-cmake/

cmake_minimum_required(VERSION 3.11...3.27)

project(
  Bison
  VERSION 1.0
  LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(PACKAGE_TESTS "Build the tests" ON)
  
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(BISON_PUBLIC_HEADERS
  "${CMAKE_CURRENT_LIST_DIR}/src/bison/bison.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/bison/bison_common.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/bison/bison_sync.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/bison/bison_serialization.hpp"
  "${CMAKE_CURRENT_LIST_DIR}/src/bison/bison_object.hpp"
)

set(BISON_PRIVATE_SOURCES
  src/bison/bison.cpp
)

include_directories(. include)

add_subdirectory("extern/json")
# Allow legacy subprojects (libyaml currently declares cmake_minimum_required 3.0).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory("extern/yaml")

if(MSYS OR CYGWIN)
  # Look for the pre-compiled system libuv provided by pacman/cygwin
  message(STATUS "Using MSYS/Cygwin system libuv via pkg-config")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUV REQUIRED libuv)
  
  # Create an interface target so you don't have to break your downstream target_link_libraries
  add_library(uv_a INTERFACE)
  target_link_libraries(uv_a INTERFACE ${LIBUV_LIBRARIES})
  target_include_directories(uv_a INTERFACE ${LIBUV_INCLUDE_DIRS})
else()
  # Fallback to the bundled source for Linux and pure Windows
  message(STATUS "Building bundled libuv from extern/")
  set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory("extern/libuv")
endif()

set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GFLAGS_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
add_subdirectory("extern/gflags")

# ─── Static library (C++ API) ────────────────────────────────────────────────
add_library(bison STATIC
  ${BISON_PUBLIC_HEADERS}
  ${BISON_PRIVATE_SOURCES}
)

target_include_directories(bison
    PRIVATE
    src/bison
    PUBLIC
        include
)

target_link_libraries(bison PRIVATE nlohmann_json::nlohmann_json yaml gflags)
target_link_libraries(bison PRIVATE uv_a)
# Required when this static library is linked into a shared library.
set_property(TARGET bison PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─── RMI framework sources ────────────────────────────────────────────────────
target_sources(bison PRIVATE
  src/rmi/shared/ids.cpp  src/rmi/shared/envelope.cpp
  src/rmi/transport/memory_transport.cpp
  src/rmi/transport/named_pipe_transport.cpp
  src/rmi/transport/pipe_transport.cpp
  src/rmi/transport/socket_transport.cpp
  src/rmi/transport/stream_transport.cpp
  src/rmi/transport/stdio_transport.cpp
  src/rmi/bridge/bridge.cpp
  src/rmi/client/client.cpp
  src/rmi/client/proxy.cpp
  src/rmi/server/server.cpp
  src/rmi/standalone/standalone.cpp
  src/app/client/client_app.cpp
  src/app/cli/cli_app.cpp
  src/app/server/server_app.cpp
  src/app/transport_flags.cpp
  src/app/debugger.cpp
  src/pty/crlf_output_guard.cpp
  src/pty/pty_write.cpp
)

target_include_directories(bison PRIVATE src/rmi)

if(WIN32)
  target_compile_definitions(bison PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ─── pty support (src/pty) — Linux and MSYS2 only ──────────────────────────
target_sources(bison PRIVATE src/pty/pty_process.cpp src/pty/raw_mode_guard.cpp)
target_link_libraries(bison PRIVATE util)

# ─── Shared library (pure-C ABI) ───────────────────────────────────────────
add_library(bison_abi SHARED
  include/bison_c.h
  include/rmi_c.h
  src/bison/bison_c.cpp
  src/rmi/rmi_c.cpp
  src/app/abi_flags.cpp
  src/app/debugger.cpp
)

target_compile_definitions(bison_abi PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_abi
    PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE
  src/rmi
)
target_link_libraries(bison_abi PRIVATE bison nlohmann_json::nlohmann_json yaml gflags)
target_link_libraries(bison_abi PRIVATE util)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
add_subdirectory(src/srv)

# ─── bison-cli executable ─────────────────────────────────────────────────────
add_executable(bison-cli src/app/cli/main.cpp)
target_link_libraries(bison-cli PRIVATE bison nlohmann_json::nlohmann_json gflags)
target_include_directories(bison-cli PRIVATE src/rmi)
