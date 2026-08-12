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

# ── Sanitizers ────────────────────────────────────────────────────────────────
# Opt-in Clang/GCC sanitizer build, e.g. `cmake -B build -DBISON_SANITIZE=thread`.
# Used to validate concurrent code (the RMI transport/server threading model)
# that unit tests alone can't reliably catch races in. Applied globally, before
# any add_subdirectory() calls, so bundled dependencies (libuv, mbedtls, ...)
# are instrumented too -- otherwise a race entirely inside a non-instrumented
# dependency would go undetected.
set(BISON_SANITIZE "" CACHE STRING "Build with a sanitizer: thread, address, undefined, or empty for none")
if(BISON_SANITIZE)
  message(STATUS "Building with sanitizer: ${BISON_SANITIZE}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=${BISON_SANITIZE} -fno-omit-frame-pointer -g -O1")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=${BISON_SANITIZE} -fno-omit-frame-pointer -g -O1")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=${BISON_SANITIZE}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=${BISON_SANITIZE}")
endif()

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
  # Fallback to the bundled source for Linux, pure Windows, and Android (the
  # NDK sysroot has no pkg-config'able system libuv to find).
  message(STATUS "Building bundled libuv from extern/")
  set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  add_subdirectory("extern/libuv")
endif()

set(GFLAGS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GFLAGS_BUILD_PACKAGING OFF CACHE BOOL "" FORCE)
add_subdirectory("extern/gflags")

# mbedTLS: static-only, no bundled programs/tests/docs (see src/rmi/DESIGN.md
# §3.4 for why mbedTLS was chosen for the tls_socket_transport dependency).
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
# Enables MBEDTLS_THREADING_C/MBEDTLS_THREADING_PTHREAD -- required for
# mbedTLS's own internal shared state (PSA key slots, lazily-initialized
# ciphersuite tables) to be thread-safe, which tls_socket_transport's
# concurrent-handshake design (see cmake/mbedtls_user_config.h) depends on.
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
add_subdirectory("extern/mbedtls")

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
target_link_libraries(bison PRIVATE mbedtls mbedx509 mbedcrypto)
# Required when this static library is linked into a shared library.
set_property(TARGET bison PROPERTY POSITION_INDEPENDENT_CODE ON)

# ─── RMI framework sources ────────────────────────────────────────────────────
target_sources(bison PRIVATE
  src/rmi/shared/ids.cpp  src/rmi/shared/envelope.cpp
  src/rmi/transport/memory_transport.cpp
  src/rmi/transport/named_pipe_transport.cpp
  src/rmi/transport/socket_transport.cpp
  src/rmi/transport/tls_socket_transport.cpp
  src/rmi/transport/stream_transport.cpp
  src/rmi/transport/term_transport.cpp
  src/rmi/bridge/bridge.cpp
  src/rmi/client/client.cpp
  src/rmi/client/proxy.cpp
  src/rmi/server/server.cpp
  src/rmi/standalone/standalone.cpp
  src/app/bridge/bridge_app.cpp
  src/app/client/client_app.cpp
  src/app/client/line_editor.cpp
  src/app/cli/cli_app.cpp
  src/app/server/server_app.cpp
  src/app/standalone/standalone_app.cpp
  src/app/transport_flags.cpp
  src/app/downstream_transport_flags.cpp
  src/app/upstream_transport_flags.cpp
  src/term/scoped_terminal_config.cpp
)

target_include_directories(bison PRIVATE src/rmi)

# ─── Platform-specific implementation pairs ────────────────────────────────
# Bison targets Linux and MSYS2 as one shared POSIX implementation, plus a
# genuinely native (non-MSYS2/Cygwin) Windows build for the pieces that
# support it. `WIN32 AND NOT MSYS AND NOT CYGWIN` isolates that native case;
# terminal_posix.cpp/terminal_win.cpp (src/term) established this pattern,
# reused here for every other file with a real per-OS implementation.
if(WIN32 AND NOT MSYS AND NOT CYGWIN)
  # _WIN32_WINNT=0x0A00 (Windows 10): terminal_win.cpp uses the ConPTY APIs
  # (CreatePseudoConsole et al.), which mingw-w64's headers only declare when
  # targeting Windows 10 1809+ (NTDDI_WIN10_RS5). The toolchain's default
  # _WIN32_WINNT (0x0601, Windows 7) hides those declarations.
  target_compile_definitions(bison PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN BISON_NATIVE_WINDOWS _WIN32_WINNT=0x0A00)
  target_sources(bison PRIVATE
    src/term/terminal_win.cpp
    src/term/scoped_terminal_config_win.cpp
    src/app/debugger_win.cpp
    src/app/client/line_editor_win.cpp
  )
else()
  if(WIN32)
    target_compile_definitions(bison PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  target_sources(bison PRIVATE
    src/term/terminal_posix.cpp
    src/term/scoped_terminal_config_posix.cpp
    src/app/debugger_posix.cpp
    src/app/client/line_editor_posix.cpp
  )
endif()

# ─── Shared library (pure-C ABI) ───────────────────────────────────────────
add_library(bison_abi SHARED
  include/bison_c.h
  include/rmi_c.h
  src/bison/bison_c.cpp
  src/rmi/rmi_c.cpp
  src/app/abi_flags.cpp
)

if(WIN32 AND NOT MSYS AND NOT CYGWIN)
  target_compile_definitions(bison_abi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN BISON_NATIVE_WINDOWS _WIN32_WINNT=0x0A00)
  target_sources(bison_abi PRIVATE src/app/debugger_win.cpp)
else()
  if(WIN32)
    target_compile_definitions(bison_abi PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  target_sources(bison_abi PRIVATE src/app/debugger_posix.cpp)
endif()

target_compile_definitions(bison_abi PRIVATE BISON_BUILDING_DLL)
target_include_directories(bison_abi
    PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}
    PRIVATE
  src/rmi
)
target_link_libraries(bison_abi PRIVATE bison nlohmann_json::nlohmann_json yaml gflags mbedtls mbedx509 mbedcrypto)
# glibc (Linux) and MSYS2's runtime split forkpty()/openpty() (used by
# src/term/terminal_posix.cpp, pulled in via term_transport's
# rmi_server_term_create()) into a separate libutil that must be linked
# explicitly. Bionic (Android's libc) has never had that split -- it has
# exported forkpty()/openpty() directly from libc.so since API 23, and the
# NDK sysroot ships no libutil.so to link against -- so Android is excluded
# here the same way native Windows already is.
if(NOT (WIN32 AND NOT MSYS AND NOT CYGWIN) AND NOT ANDROID)
  target_link_libraries(bison_abi PRIVATE util)
endif()

# Release packaging (see cmake/Packaging.cmake for the rest -- docs,
# bindings, CPack ZIP config). Tagged COMPONENT bison so a project
# consuming bison via add_subdirectory() doesn't get these mixed into its
# own default install component.
install(TARGETS bison_abi
    RUNTIME DESTINATION bin COMPONENT bison   # bison_abi.dll (Windows/MSYS2)
    LIBRARY DESTINATION bin COMPONENT bison   # libbison_abi.so (Linux)
    ARCHIVE DESTINATION lib COMPONENT bison)  # bison_abi.lib (MSVC import lib)
install(FILES include/bison_c.h include/rmi_c.h
    DESTINATION include
    COMPONENT bison)

if(PACKAGE_TESTS)
    enable_testing()
    include(GoogleTest)
    add_subdirectory(tests)
endif()

add_subdirectory(examples)
add_subdirectory(src/srv)
add_subdirectory(bindings/cpp)

# ─── Android binding (JNI) ─────────────────────────────────────────────────
# Only configured when cross-compiling with the NDK's CMake toolchain file
# (which sets ANDROID); the JNI glue needs <jni.h>, which the NDK sysroot
# provides for free, and has no reason to exist in a host Linux/Windows
# build. See docs/bindings.md for the binding itself and docs/building.md
# for the NDK toolchain invocation.
if(ANDROID)
  add_subdirectory(bindings/android/jni)
endif()

# ─── bison-cli executable ─────────────────────────────────────────────────────
add_executable(bison-cli src/app/cli/main.cpp)
target_link_libraries(bison-cli PRIVATE bison nlohmann_json::nlohmann_json gflags)
target_include_directories(bison-cli PRIVATE src/rmi)
install(TARGETS bison-cli RUNTIME DESTINATION bin COMPONENT bison)

# Non-target install() rules (docs, license, binding sources/examples) plus
# the CPack ZIP config live in cmake/Packaging.cmake. Only include it when
# bison is the top-level project -- wish (and any other consumer) pulls
# bison in via add_subdirectory("extern/bison"), and CPack's include(CPack)
# is a global singleton (a second include() from wish's own packaging file
# would silently no-op), so this must not run in that context.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  include(cmake/Packaging.cmake)
endif()
