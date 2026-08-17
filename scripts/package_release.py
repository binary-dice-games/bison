#!/usr/bin/env python3
"""Build bison and produce a release zip for GitHub Releases.

This is a thin orchestrator around the packaging CMake already knows how to
do (see cmake/Packaging.cmake): configure a Release build, build it, run
`cpack` to get a ZIP containing everything install()-tagged COMPONENT bison
(bison-cli, bison_abi + headers, docs, binding sources/examples -- examples/
and src/srv/calc's calc-server are deliberately excluded, they're
demo/tutorial code, not something an end user of the ABI/CLI needs), then
bolt on the handful of things that aren't CMake build-tree artifacts:

  - the C# binding compiled to a DLL via `dotnet publish`
  - on MSYS2/Windows, the mingw runtime DLLs the built binaries dynamically
    link against (the project has no static-linking flags, so a bare
    Windows machine without MSYS2 installed would otherwise be missing
    libstdc++-6.dll and friends)

Usage:
    python3 scripts/package_release.py
    python3 scripts/package_release.py --version 1.2.3 --build-dir build-release
"""

import argparse
import os
import platform
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def default_generator() -> str:
    # Ninja needs its own executable on PATH plus an MSVC dev environment
    # (vcvars) already loaded to find cl.exe -- neither is set up on a bare
    # native Windows machine. A genuine MSYS2 shell provides its own
    # pacman-installed gcc/Ninja, same POSIX toolchain as Linux, so it's
    # fine to keep using Ninja there. Checking MSYSTEM alone isn't enough,
    # though: Git for Windows' bundled Git Bash also sets MSYSTEM (e.g.
    # MINGW64) but ships no compiler of its own -- gcc on PATH is what
    # actually distinguishes a real MSYS2 dev install from that.
    if platform.system() == "Windows" and not (
        os.environ.get("MSYSTEM") and shutil.which("gcc")
    ):
        return "Visual Studio 17 2022"
    return "Ninja"


def run(cmd, cwd=None):
    print(f"+ {' '.join(str(c) for c in cmd)}")
    subprocess.run(cmd, cwd=cwd, check=True)


def configure(build_dir: Path, generator: str, version: str | None):
    args = [
        "cmake", "-S", str(REPO_ROOT), "-B", str(build_dir),
        "-G", generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DPACKAGE_TESTS=OFF",
    ]
    if version:
        args.append(f"-DBISON_PACKAGE_VERSION={version}")
    run(args)


def build(build_dir: Path):
    run(["cmake", "--build", str(build_dir), "--config", "Release",
         "--parallel"])


def cpack(build_dir: Path) -> Path:
    # -C Release matters only for multi-config generators (Visual Studio);
    # single-config generators (Ninja/Makefiles) already baked the config
    # into CMAKE_BUILD_TYPE at configure time and ignore this flag.
    result = subprocess.run(
        ["cpack", "-G", "ZIP", "-C", "Release"], cwd=build_dir, check=True,
        capture_output=True, text=True,
    )
    print(result.stdout)
    match = re.search(r"package: (.+\.zip) generated", result.stdout)
    if not match:
        raise RuntimeError("cpack did not report a generated package path")
    return Path(match.group(1))


def stage_zip(zip_path: Path, staging_dir: Path):
    if staging_dir.exists():
        shutil.rmtree(staging_dir)
    staging_dir.mkdir(parents=True)
    with zipfile.ZipFile(zip_path) as zf:
        zf.extractall(staging_dir)
        # zipfile.extractall() silently drops Unix permission bits (a
        # long-standing stdlib gotcha: ZipInfo.external_attr stores them,
        # but extraction never applies them) -- without this, every
        # executable CPack correctly marked +x (binaries, bison-env.sh,
        # install.sh) comes out of the zip as plain rw-r--r--.
        if platform.system() != "Windows":
            for info in zf.infolist():
                mode = (info.external_attr >> 16) & 0o777
                if mode:
                    (staging_dir / info.filename).chmod(mode)


def bundle_csharp_dlls(staging_dir: Path):
    if shutil.which("dotnet") is None:
        print("warning: dotnet not found on PATH, skipping C# DLL bundle "
              "(bindings/csharp/src/Bison/ source is still in the zip)")
        return
    csproj = REPO_ROOT / "bindings" / "csharp" / "Bison" / "Bison.csproj"
    out_dir = staging_dir / "bindings" / "csharp" / "lib"
    run(["dotnet", "publish", str(csproj), "-c", "Release", "-o", str(out_dir)])
    # dotnet publish drops a lot of framework-dependent scaffolding; keep only
    # what a consumer actually links against.
    for f in out_dir.iterdir():
        if f.is_file() and not f.name.startswith("Bdg.Bison"):
            f.unlink()
    print(f"bundled C# binding DLL -> {out_dir}")


def bundle_mingw_runtime_dlls(staging_dir: Path):
    if shutil.which("ldd") is None:
        print("warning: ldd not found, skipping MSYS2 runtime DLL bundle "
              "(zip will require an MSYS2 shell/PATH to run)")
        return
    bin_dir = staging_dir / "bin"
    binaries = list(bin_dir.glob("*.exe")) + list(bin_dir.glob("*.dll"))
    needed = set()
    for binary in binaries:
        result = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            match = re.search(r"=> (/mingw64/bin/\S+)", line)
            if match:
                needed.add(Path(match.group(1)))
    for dll in sorted(needed):
        dst = bin_dir / dll.name
        if not dst.exists():
            shutil.copy2(dll, dst)
            print(f"bundled runtime dependency {dll.name}")


def make_final_zip(staging_dir: Path, output_dir: Path, version: str, arch: str) -> Path:
    output_dir.mkdir(parents=True, exist_ok=True)
    system = platform.system()
    final_path = output_dir / f"bison-{version}-{system}-{arch}.zip"
    if final_path.exists():
        final_path.unlink()
    with zipfile.ZipFile(final_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for file in sorted(staging_dir.rglob("*")):
            if file.is_file():
                zf.write(file, file.relative_to(staging_dir))
    return final_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build-release",
                         help="CMake build directory (default: build-release; "
                              "kept separate from the developer build/ dirs)")
    parser.add_argument("--generator", default=None,
                         help="CMake generator (default: Visual Studio 17 "
                              "2022 on native Windows, Ninja elsewhere)")
    parser.add_argument("--output-dir", default="dist")
    parser.add_argument("--version", default=None,
                         help="Release version, e.g. 1.2.3 (default: the "
                              "CMake project version)")
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    build_dir = (REPO_ROOT / args.build_dir).resolve()
    output_dir = (REPO_ROOT / args.output_dir).resolve()

    generator = args.generator or default_generator()
    if not args.skip_configure:
        configure(build_dir, generator, args.version)
    if not args.skip_build:
        build(build_dir)

    zip_path = cpack(build_dir)
    version = args.version or re.match(r"bison-(.+?)-", zip_path.name).group(1)
    arch = platform.machine()

    staging_dir = build_dir / "release-staging"
    stage_zip(zip_path, staging_dir)

    bundle_csharp_dlls(staging_dir)
    if platform.system() == "Windows" or os.environ.get("MSYSTEM"):
        bundle_mingw_runtime_dlls(staging_dir)

    final_path = make_final_zip(staging_dir, output_dir, version, arch)
    print(f"\nRelease zip: {final_path}")


if __name__ == "__main__":
    main()
