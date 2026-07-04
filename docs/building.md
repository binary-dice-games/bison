# Building Bison

Bison targets **Linux and MSYS2 only**; native Windows/MSVC builds are not
supported. See below for setup on Linux, WSL, or MSYS2.

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

```cmake
add_subdirectory(bison)
target_link_libraries(my_target PRIVATE bison)
```

```cpp
#include <bison.hpp>
```

## Building on Linux / WSL / MSYS2

Server-side `--pty` mode (forking a real pseudo-terminal via `pty_process`,
see `src/pty/DESIGN.md`) requires `forkpty()`, so it needs a POSIX layer —
either Linux (natively or via WSL) or MSYS2 on Windows. The
`stdio_*_transport` framing itself (`--pipe`/`--pty` on the client side) is
cross-platform.

Server-side `--transport=console` (spawning `--cmd` via libuv's `uv_spawn`,
see `src/console/console_process.hpp`) doesn't need `forkpty()`, but it
still execs via `/bin/sh -c`, so it inherits the same `/bin/sh` resolution
requirement as `--pty` on MSYS2 — see the MSYS2 note below.

### Building on MSYS2 (native Windows)

MSYS2 provides `forkpty()` without needing a WSL VM. Install it from
https://www.msys2.org/, then from an **MSYS2 MSYS** shell (not MinGW64,
not a plain `cmd.exe`/PowerShell prompt):

```bash
pacman -S --needed mingw-w64-x86_64-cmake ninja libuv-devel
cd /c/github/bison   # or wherever the repo lives
cmake -B build-msys2 -G Ninja -DPACKAGE_TESTS=ON
cmake --build build-msys2
ctest --test-dir build-msys2 --output-on-failure
```

**Every `--pty`- or `--transport=console`-capable executable
(`rmi_server_example`, `rmi_client_example`, `pty_process_test`,
`console_process_test`, etc.) must be launched from an MSYS2 MSYS shell**,
i.e. one whose `PATH` includes the real `.../usr/bin` from the MSYS2
install. This is a hard requirement, not a convenience: MSYS2's
`msys-2.0.dll` derives its POSIX path-mount table (including the virtual
`/bin` → `/usr/bin` alias that `execl("/bin/sh", ...)`/`uv_spawn("/bin/sh", ...)`
rely on) from *where that DLL itself was loaded from*. If it's loaded from
anywhere other than a real `usr/bin` under the MSYS2 root — e.g. a copy
sitting next to the built `.exe` — the mount table can't be built,
`/bin/sh` fails to resolve, the forked pty/console child exits immediately
with status 127, and the whole program exits right after printing its
startup banner (looks like the console flashing open and closed). For this
reason the build does **not** copy MSYS runtime DLLs next to build
outputs — rely on `PATH` from a real MSYS2 shell instead:

```bash
cd build-msys2/examples
./rmi_server_example.exe --pty
```

Running the same `.exe` from a plain PowerShell/cmd window without MSYS2's
`usr/bin` on `PATH` will fail for the reason above.

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
