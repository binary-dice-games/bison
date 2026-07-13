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
