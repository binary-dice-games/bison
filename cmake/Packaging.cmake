# cmake/Packaging.cmake
#
# Release-zip packaging: install() destinations for content that isn't a
# CMake build target (docs, license, binding sources/examples), plus the
# CPack ZIP config that turns the whole install tree into a distributable
# archive. Target-owned install() rules (bison-cli, bison_abi, public C
# headers) live next to their targets in the root CMakeLists.txt -- this
# file only adds what has nowhere else to live. Deliberately excludes
# examples/ and src/srv/calc (calc-server) -- those are demo/tutorial code,
# not something an end user consuming the ABI/CLI needs.
#
# Produces a zip via:
#   cmake --build <build-dir> --target package
# or, equivalently, running `cpack` from inside <build-dir>. See
# scripts/package_release.py for a full configure+build+package+post-process
# orchestrator (it also bundles the compiled C# binding and, on
# MSYS2/Windows, the runtime DLLs CPack has no way to know about).

install(FILES README.md LICENSE DESTINATION . COMPONENT bison)
install(DIRECTORY docs DESTINATION . COMPONENT bison)

# bindings/python is a plain importable package (no setup.py/pyproject.toml
# in this repo -- see docs/bindings.md); ship the source and examples
# as-is, minus stray bytecode caches.
install(DIRECTORY bindings/python/bison DESTINATION bindings/python
    COMPONENT bison
    PATTERN "__pycache__" EXCLUDE)
install(DIRECTORY bindings/python/examples DESTINATION bindings/python
    COMPONENT bison
    PATTERN "__pycache__" EXCLUDE)

# bindings/csharp/Bison/Bison.csproj has no external ProjectReference (self-
# contained, unlike wish's C# binding which references this very project),
# so the source alone is standalone-buildable. Ship it anyway alongside the
# compiled DLL scripts/package_release.py additionally `dotnet publish`es
# into bindings/csharp/lib/, for consumers who just want to reference the
# library without building it themselves.
install(DIRECTORY bindings/csharp/Bison DESTINATION bindings/csharp/src
    COMPONENT bison)
install(DIRECTORY bindings/csharp/examples DESTINATION bindings/csharp
    COMPONENT bison)

# PATH/library-path registration helpers, so `bison-cli` and
# bison_abi.dll/libbison_abi.so are discoverable from anywhere in the
# filesystem after extracting the zip, not just from inside bin/. See
# packaging/{unix,windows}/*'s own comments for the platform-specific
# reasoning (Windows' DLL search consults PATH; Linux's dynamic linker does
# not, hence LD_LIBRARY_PATH/BISON_LIB on top of PATH there).
if(WIN32)
  install(PROGRAMS
      packaging/windows/bison-env.ps1
      packaging/windows/bison-env.cmd
      packaging/windows/install.ps1
      DESTINATION .
      COMPONENT bison)
else()
  install(PROGRAMS
      packaging/unix/bison-env.sh
      packaging/unix/install.sh
      DESTINATION .
      COMPONENT bison)
endif()

# ── CPack ────────────────────────────────────────────────────────────────
#
# All of this project's own install() rules (above, and the target-owned
# ones in the root CMakeLists.txt) are tagged COMPONENT bison.
# FetchContent/add_subdirectory-vendored dependencies (nlohmann_json, yaml,
# libuv, gflags, googletest, ...) bring their own install() rules in as an
# untagged/default component -- restricting packaging to just the "bison"
# component keeps the release zip to bison's own files instead of every
# dependency's dev artifacts.

set(CPACK_COMPONENTS_ALL bison)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL_IN_ONE_PACKAGE ON)

set(CPACK_PACKAGE_NAME "bison")
set(CPACK_PACKAGE_VENDOR "Binary Dice Games")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "bison -- a C++ RMI/serialization framework")

# BISON_PACKAGE_VERSION lets a release workflow stamp the zip with a tag
# version (e.g. "1.2.3") independent of the CMake project() VERSION, which
# is a slow-moving API-compatibility marker, not a release number.
if(NOT BISON_PACKAGE_VERSION)
  set(BISON_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()
set(CPACK_PACKAGE_VERSION "${BISON_PACKAGE_VERSION}")

set(CPACK_PACKAGE_FILE_NAME
    "bison-${BISON_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_VERBATIM_VARIABLES TRUE)

include(CPack)
