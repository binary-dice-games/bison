# Building Bison

## Quick Build (Windows)

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

```cmake
add_subdirectory(bison)
target_link_libraries(my_target PRIVATE bison)
```

```cpp
#include <bison.hpp>
```

## Building on Linux / WSL

The stdio PTY transport, Linux-specific examples, and unit tests require Linux. Use WSL on Windows 10 v2004+ or Windows 11.

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
