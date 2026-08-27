# Building Bison

Bison supports three platform configurations: **Linux**, **MSYS2**, and
**native Windows** (MSVC or mingw64 — not MSYS2/Cygwin). Linux and MSYS2
share one POSIX implementation (`forkpty()`, `termios`, etc.); native
Windows uses a separate implementation (ConPTY, `IsDebuggerPresent()`, etc.)
selected automatically by CMake. See below for setup on Linux, WSL, MSYS2,
or native Windows.

## Quick Build

```bash
git clone --recurse-submodules https://github.com/carloslopezmdez/bison.git
cd bison
cmake -B build
cmake --build build --config Debug

# With tests
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug
```

## CMake Integration

`target_link_libraries` pulls in the `bison` static library, but its headers
are consumed by path from the repo root (there is no installed/flattened
public include tree yet), so add the cloned directory to your include path
too:

```cmake
add_subdirectory(bison)
target_link_libraries(my_target PRIVATE bison)
target_include_directories(my_target PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bison)
```

```cpp
#include "src/bison/bison.hpp"   // core dynamic-object API
#include "src/rmi/rmi.hpp"       // RMI framework (optional)
```

Prefer a stable ABI boundary instead? Link `bison_abi` (a shared library)
and include `bison_c.h`/`rmi_c.h` from `include/` instead — see
[docs/bindings.md](bindings.md), which wraps that same C ABI.

### Building on WSL

### 1 — Install WSL

```powershell
wsl --install
# Check existing distributions:
wsl --list --verbose
# Install specific distro (Ubuntu 22.04 recommended):
wsl --install -d Ubuntu-22.04
```

### 2 — Install build tools

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y build-essential cmake ninja-build git
```

Verify `cmake --version` is 3.11 or later.

### 3 — Access the repository

The Windows filesystem mounts under `/mnt/c/`, `/mnt/d/`, etc. If the repo is at `D:\github\bison`:

```bash
cd /mnt/d/github/bison
```

Or clone directly inside WSL for better I/O performance:

```bash
cd ~
git clone --recurse-submodules https://github.com/carloslopezmdez/bison.git
cd bison
```

### 4 — Configure and build

Use a separate build directory to avoid conflicting with the Windows `build/` folder:

```bash
cmake -B build-linux -G Ninja -DPACKAGE_TESTS=ON
cmake --build build-linux

# Build specific targets only:
cmake --build build-linux --target bison_test rmi_test bison_c_test rmi_c_test
```

### 5 — Run tests

```bash
ctest --test-dir build-linux --output-on-failure

# Or run individual test executables:
./build-linux/tests/bison_test
./build-linux/tests/rmi_test --gtest_filter="*RMI*"
```

### Building on Native Windows (MSVC or mingw64)

Native Windows is a genuinely separate build target from MSYS2/Cygwin — it
uses ConPTY instead of `forkpty()`, `IsDebuggerPresent()` instead of
`/proc/self/status`, etc. CMake detects it automatically via
`WIN32 AND NOT MSYS AND NOT CYGWIN` and selects the right sources; no extra
flags are needed.

```powershell
git clone --recurse-submodules https://github.com/carloslopezmdez/bison.git
cd bison

# MSVC (Visual Studio generator, multi-config):
cmake -B build -DPACKAGE_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug

# mingw64 (e.g. from MSYS2's mingw64 shell, single-config):
cmake -B build-mingw64 -G Ninja -DPACKAGE_TESTS=ON
cmake --build build-mingw64
ctest --test-dir build-mingw64
```

The native vs. MSYS2 code path is selected by whether CMake itself reports
`MSYS`/`CYGWIN` (true only when invoked via the MSYS2/Cygwin-packaged
`cmake`), not by which compiler is used — a native (non-MSYS2) `cmake.exe`
targeting mingw64 still takes the native Windows path described above. For
the MSYS2 path instead (sharing the POSIX implementation with Linux), see
"Building on WSL" above, run from an MSYS2 shell.

### Building for Android

Android is cross-compiled with the NDK's own CMake toolchain file, same as
any other NDK-based CMake project — nothing Bison-specific to invoke beyond
pointing `-DCMAKE_TOOLCHAIN_FILE` at it. CMake's Android toolchain sets the
`ANDROID` variable, which selects the POSIX code path everywhere the rest of
this document's platform matrix branches on `WIN32`/`MSYS`/`CYGWIN` (Android
takes the same `terminal_posix.cpp`/`debugger_posix.cpp` sources Linux and
MSYS2 do), with one exception: `bison_abi` skips linking `-lutil`. Linux and
MSYS2 need it for `forkpty()`/`openpty()` (pulled in transitively by
`term_transport`'s `rmi_server_term_create()`); Bionic (Android's libc) has
exported those directly from `libc.so` since API 23 and the NDK sysroot ships
no `libutil.so` to link against, so `CMakeLists.txt` excludes Android from
that `target_link_libraries(bison_abi PRIVATE util)` call the same way it
already excludes native Windows.

```bash
# From an NDK install (r26+; set ANDROID_NDK_ROOT to its path):
cmake -B build-android-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DPACKAGE_TESTS=OFF
cmake --build build-android-arm64 --target bison_abi bison_jni

# For the emulator (x86_64):
cmake -B build-android-x86_64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=x86_64 \
    -DANDROID_PLATFORM=android-24 \
    -DPACKAGE_TESTS=OFF
cmake --build build-android-x86_64 --target bison_abi bison_jni
```

`bison_jni` (`bindings/android/jni/`, the Java/Kotlin binding's JNI glue) is
only configured when `ANDROID` is set — it needs `<jni.h>`, which the NDK
sysroot provides, and has no reason to exist in a host build. `-DPACKAGE_TESTS=OFF`
skips configuring GoogleTest and the test suite, which this cross build has
no use for.

In practice this whole invocation is driven by Gradle's `externalNativeBuild`
instead of by hand — `bindings/android/bison-lib/build.gradle` points
straight at this repo's root `CMakeLists.txt` and restricts the build to the
`bison_abi`/`bison_jni` targets, so `./gradlew assembleDebug` (or an
Android Studio sync) runs the equivalent of the commands above once per
`abiFilters` entry automatically. See [docs/bindings.md](bindings.md) for the
binding itself and [docs/examples.md](examples.md) for building/running the
example app on an emulator.

---

## Packaging a release

`cmake/Packaging.cmake` adds `install()` rules (tagged `COMPONENT bison`, to
keep add_subdirectory-vendored dependencies like nlohmann_json/libuv out of
the package) plus a CPack `ZIP` generator config, so any configured build
can produce a release zip directly:

```sh
cmake --build build --target package   # or: cd build && cpack -G ZIP
```

That zip has `bison-cli`, `bison_abi` + its public C headers (`bison_c.h`,
`rmi_c.h`), `docs/` + `README.md`, and the Python/C# binding sources and
examples — deliberately excluding `examples/`'s and `src/srv/calc`'s own
binaries (`bison_examples`, `calc-server`, etc.), which are demo/tutorial
code, not something an end user of the ABI/CLI needs.
`scripts/package_release.py` wraps this end to end — configuring a Release
build, building, running `cpack`, then bolting on the pieces CPack can't
produce on its own: a compiled C# binding DLL (via `dotnet publish`, if
`dotnet` is on `PATH`) and, on MSYS2/native Windows, the runtime DLLs the
binaries dynamically link against (via `ldd`):

```sh
python3 scripts/package_release.py --version 1.2.3
```

Produces `dist/bison-<version>-<System>-<arch>.zip`. Run it once per
platform (Linux, MSYS2, native Windows) to produce that platform's release
asset.

On a published GitHub Release this runs automatically:
`.github/workflows/release-binaries.yml` builds and validates the zip on
Linux (x86_64 + aarch64), macOS arm64, and native Windows (MSVC), and
attaches all four to the release. See
[docs/publishing-binaries.md](publishing-binaries.md) for the runbook.

### Putting the release on PATH

The zip also ships a pair of small scripts (`packaging/unix/` on Linux,
`packaging/windows/` on Windows — installed to the zip's root) so
`bison-cli` and `bison_abi.dll`/`libbison_abi.so` are usable from any
working directory after extracting, not just from inside the zip's own
`bin/`:

- **`bison-env.sh` / `bison-env.ps1` / `bison-env.cmd`** — session-only, no
  files modified. `source ./bison-env.sh` (Linux), `. .\bison-env.ps1`
  (PowerShell), or `bison-env.cmd` (cmd.exe).
- **`install.sh` / `install.ps1`** — persists the change so new terminals
  pick it up automatically: appends a marked, idempotent block to
  `~/.bashrc`/`~/.zshrc` on Linux, or (on Windows, via `install.ps1`) adds
  the zip's `bin\` to the per-user `HKCU` `PATH` — no administrator rights
  needed, and never touches the machine-wide PATH. Both accept
  `--uninstall` / `-Uninstall` to remove what they added.

Windows searches every directory on `PATH` when resolving a DLL, so `PATH`
alone covers both running `bison-cli.exe` from anywhere and a separate
program (a C# app P/Invoking `bison_abi.dll`, Python
`ctypes.CDLL("bison_abi.dll")` without `BISON_LIB` set) finding it. Linux's
dynamic linker does not consult `PATH` for shared libraries, so both
scripts additionally set `LD_LIBRARY_PATH` and `BISON_LIB` (the latter read
directly by `bindings/python/bison/_native.py`) there.
