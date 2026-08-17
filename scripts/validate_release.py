#!/usr/bin/env python3
"""Sanity-check a release zip produced by scripts/package_release.py.

Run this after package_release.py and before publishing. It extracts the
zip and checks, in order:

  1. structure    -- the expected top-level entries are present
  2. permissions  -- bin/bison-cli, install.sh, bison-env.sh kept their +x
                      bit through the extract -> rezip round trip (skipped
                      on Windows -- not a meaningful concept there)
  3. cli-smoke    -- bin/bison-cli actually runs (--version, --help)
  4. dynamic-link -- bin/bison-cli and the bison_abi shared lib only pull in
                      system libs (no "not found" deps, nothing accidentally
                      linked from your dev build tree). On native Windows,
                      where there's no ldd, this loads bison_abi.dll through
                      the OS loader itself (ctypes.WinDLL) -- a missing
                      dependency fails the load the same way ldd flags it.
  5. abi-link     -- compiles and runs a tiny C program against the packaged
                      include/bison_c.h, linked against the packaged shared
                      lib -- this is what catches a stale-header/stale-lib
                      mismatch, the most common release packaging bug. On
                      native Windows this links against the packaged
                      bison_abi.lib import lib using cl.exe, auto-located
                      via vswhere.exe if not already in a Developer Prompt.
  6. csharp-dll   -- bindings/csharp/lib/*.dll, if present, is a real
                      assembly (not a placeholder/empty file)
  7. rmi-roundtrip -- optional (--calc-server): spins up calc-server, points
                      the packaged bison-cli at it over real TCP,
                      instantiates a Calculator and calls add(2, 5), checks
                      the result is 7. calc-server is deliberately excluded
                      from the release zip (demo/tutorial code), so this
                      needs the server binary from your build tree.

Usage:
    python3 scripts/validate_release.py dist/bison-1.2.3-Linux-x86_64.zip
    python3 scripts/validate_release.py dist/bison-1.2.3-Linux-x86_64.zip \
        --calc-server build-release/calc-server

Exits 0 if every non-skipped check passes, 1 otherwise.
"""

import argparse
import ctypes
import os
import platform
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

ABI_CHECK_SRC = """
#include "bison_c.h"
#include <stdio.h>
int main(void) {
    bison_handle h = bison_create(0);
    if (!h) { fprintf(stderr, "bison_create failed\\n"); return 1; }
    char* json = NULL;
    if (bison_to_json(h, 0, &json) != BISON_OK) {
        fprintf(stderr, "bison_to_json failed\\n");
        return 1;
    }
    printf("%s", json);
    bison_free_string(json);
    bison_release(h);
    return 0;
}
"""

REQUIRED_TOP_LEVEL = ["bin", "include", "bindings", "README.md", "LICENSE"]


class Results:
    def __init__(self):
        self.entries = []  # (name, status, detail)
        self.ok = True

    def report(self, name, status, detail=""):
        self.entries.append((name, status, detail))
        if status == "FAIL":
            self.ok = False
        marker = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "SKIP"}[status]
        line = f"[{marker}] {name}"
        if detail:
            line += f" -- {detail}"
        print(line)


def find_one(root: Path, pattern: str) -> Path | None:
    matches = sorted(root.glob(pattern))
    return matches[0] if matches else None


def check_structure(root: Path, results: Results):
    missing = [p for p in REQUIRED_TOP_LEVEL if not (root / p).exists()]
    if missing:
        results.report("structure", "FAIL", f"missing: {', '.join(missing)}")
    else:
        results.report("structure", "PASS", f"{sum(1 for _ in root.rglob('*'))} entries")


def check_permissions(root: Path, results: Results):
    if platform.system() == "Windows":
        results.report("permissions", "SKIP", "not meaningful on Windows")
        return
    cli = find_one(root, "bin/bison-cli*")
    targets = [t for t in [cli, root / "install.sh", root / "bison-env.sh"] if t]
    missing_exec = [str(t.relative_to(root)) for t in targets if t.exists() and not (t.stat().st_mode & 0o111)]
    absent = [str(t.relative_to(root)) for t in targets if not t.exists()]
    if absent:
        results.report("permissions", "FAIL", f"missing files: {', '.join(absent)}")
    elif missing_exec:
        results.report("permissions", "FAIL", f"lost +x bit: {', '.join(missing_exec)}")
    else:
        results.report("permissions", "PASS", f"{len(targets)} files kept +x")


def check_cli_smoke(root: Path, results: Results):
    cli = find_one(root, "bin/bison-cli*")
    if not cli:
        results.report("cli-smoke", "FAIL", "bin/bison-cli not found")
        return
    try:
        ver = subprocess.run([str(cli), "--version"], capture_output=True, text=True, timeout=10)
        help_ = subprocess.run([str(cli), "--help"], capture_output=True, text=True, timeout=10)
    except OSError as e:
        results.report("cli-smoke", "FAIL", f"could not execute: {e}")
        return
    if ver.returncode != 0 or "bison-cli" not in ver.stdout:
        results.report("cli-smoke", "FAIL", f"--version failed (rc={ver.returncode}): {ver.stderr.strip()}")
        return
    if help_.returncode != 0 or "--transport" not in help_.stdout:
        results.report("cli-smoke", "FAIL", f"--help failed (rc={help_.returncode})")
        return
    results.report("cli-smoke", "PASS", "--version and --help ran cleanly")


def check_dynamic_link_windows(root: Path, results: Results):
    # No ldd on native Windows, and dumpbin needs an MSVC dev environment
    # just to enumerate imports. Instead, ask the OS loader itself: the
    # Win32 loader resolves a DLL's full dependency chain when it's loaded,
    # so a failed LoadLibrary (surfaced by ctypes as OSError/WinError) means
    # exactly what "not found" in ldd's output means -- a missing dependency
    # (e.g. a stray link against a dev-tree DLL not present in the release
    # zip). bison-cli.exe itself is exercised the same way by cli-smoke
    # actually running it, so it isn't re-checked here.
    dll = find_one(root, "bin/*bison_abi*.dll")
    if not dll:
        results.report("dynamic-link", "FAIL", "no bison_abi DLL found to check")
        return
    try:
        handle = ctypes.WinDLL(str(dll))
    except OSError as e:
        results.report("dynamic-link", "FAIL", f"{dll.name}: failed to load -- {e}")
        return
    # WinDLL keeps the module mapped (and file-locked) for the life of the
    # process otherwise, which fails the temp-dir cleanup in main() below.
    # FreeLibrary's default ctypes prototype treats the handle as a 32-bit
    # int, which overflows on a 64-bit module handle -- set argtypes so it's
    # passed as a pointer instead.
    ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
    ctypes.windll.kernel32.FreeLibrary(handle._handle)
    results.report("dynamic-link", "PASS", f"{dll.name} loaded cleanly (all dependencies resolved)")


def check_dynamic_link(root: Path, results: Results):
    if platform.system() == "Windows":
        check_dynamic_link_windows(root, results)
        return
    if shutil.which("ldd") is None:
        results.report("dynamic-link", "SKIP", "ldd not available on this platform")
        return
    targets = [t for t in [find_one(root, "bin/bison-cli*"), find_one(root, "bin/*bison_abi*.so")] if t]
    if not targets:
        results.report("dynamic-link", "FAIL", "no binaries found to check")
        return
    problems = []
    for t in targets:
        out = subprocess.run(["ldd", str(t)], capture_output=True, text=True)
        if "not found" in out.stdout:
            problems.append(f"{t.name}: {[l for l in out.stdout.splitlines() if 'not found' in l]}")
    if problems:
        results.report("dynamic-link", "FAIL", "; ".join(problems))
    else:
        results.report("dynamic-link", "PASS", f"{len(targets)} binaries resolve cleanly")


def find_vcvarsall() -> Path | None:
    """Locate vcvarsall.bat via vswhere, for when cl.exe isn't already on
    PATH (i.e. this script wasn't launched from a VS Developer Prompt)."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    result = subprocess.run(
        [str(vswhere), "-latest", "-products", "*",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property", "installationPath"],
        capture_output=True, text=True,
    )
    lines = [l for l in result.stdout.splitlines() if l.strip()]
    if not lines:
        return None
    vcvarsall = Path(lines[0]) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
    return vcvarsall if vcvarsall.exists() else None


def check_abi_link_windows(root: Path, results: Results):
    header = root / "include" / "bison_c.h"
    dll = find_one(root, "bin/*bison_abi*.dll")
    lib = find_one(root, "lib/*bison_abi*.lib")
    if not header.exists() or not dll or not lib:
        results.report("abi-link", "SKIP", "missing header, DLL, or import lib")
        return
    vcvarsall = None if shutil.which("cl") else find_vcvarsall()
    if not shutil.which("cl") and not vcvarsall:
        results.report("abi-link", "SKIP", "no MSVC compiler (cl.exe) on PATH and vcvarsall.bat could not be located")
        return
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        src = td_path / "abi_check.c"
        exe = td_path / "abi_check.exe"
        src.write_text(ABI_CHECK_SRC)
        # No rpath on Windows -- the loader searches next to the exe.
        shutil.copy2(dll, td_path / dll.name)
        cl_cmd = f'cl /nologo "{src}" /I "{root / "include"}" /Fe:"{exe}" /link "{lib}"'
        full_cmd = f'call "{vcvarsall}" x64 && {cl_cmd}' if vcvarsall else cl_cmd
        comp = subprocess.run(full_cmd, shell=True, capture_output=True, text=True, cwd=td_path)
        if comp.returncode != 0 or not exe.exists():
            results.report("abi-link", "FAIL", f"compile/link failed: {(comp.stderr or comp.stdout).strip()[:300]}")
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10, cwd=td_path)
        if run.returncode != 0 or "{}" not in run.stdout:
            results.report("abi-link", "FAIL", f"packaged header/lib mismatch: {run.stderr.strip()[:300]}")
            return
    results.report("abi-link", "PASS", "compiled against packaged header, linked packaged import lib, ran correctly")


def check_abi_link(root: Path, results: Results):
    if platform.system() == "Windows":
        check_abi_link_windows(root, results)
        return
    header = root / "include" / "bison_c.h"
    lib = find_one(root, "bin/*bison_abi*.so")
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not header.exists() or not lib or not cc:
        results.report("abi-link", "SKIP", "missing header, shared lib, or no C compiler on PATH")
        return
    with tempfile.TemporaryDirectory() as td:
        src = Path(td) / "abi_check.c"
        exe = Path(td) / "abi_check"
        src.write_text(ABI_CHECK_SRC)
        compile_cmd = [
            cc, str(src), "-I", str(root / "include"), "-L", str(lib.parent),
            "-lbison_abi", "-Wl,-rpath," + str(lib.parent), "-o", str(exe),
        ]
        comp = subprocess.run(compile_cmd, capture_output=True, text=True)
        if comp.returncode != 0:
            results.report("abi-link", "FAIL", f"compile/link failed: {comp.stderr.strip()[:300]}")
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        if run.returncode != 0 or "{}" not in run.stdout:
            results.report("abi-link", "FAIL", f"packaged header/lib mismatch: {run.stderr.strip()[:300]}")
            return
    results.report("abi-link", "PASS", "compiled against packaged header, linked packaged lib, ran correctly")


def check_csharp_dll(root: Path, results: Results):
    dll = find_one(root, "bindings/csharp/lib/*.dll")
    if not dll:
        results.report("csharp-dll", "SKIP", "no C# DLL in this archive (dotnet not available at package time?)")
        return
    data = dll.read_bytes()
    if not data.startswith(b"MZ") or len(data) < 1024:
        results.report("csharp-dll", "FAIL", f"{dll.name} does not look like a valid PE assembly")
        return
    results.report("csharp-dll", "PASS", f"{dll.name} is a valid PE assembly ({len(data)} bytes)")


def free_tcp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def check_rmi_roundtrip(root: Path, calc_server: Path, results: Results):
    cli = find_one(root, "bin/bison-cli*")
    if not cli:
        results.report("rmi-roundtrip", "FAIL", "bin/bison-cli not found")
        return
    if not calc_server.exists():
        results.report("rmi-roundtrip", "FAIL", f"--calc-server path does not exist: {calc_server}")
        return

    port = free_tcp_port()
    # calc-server blocks reading a line from stdin to know when to stop;
    # stdin=PIPE (left open, never written to) keeps it running instead of
    # exiting immediately on EOF.
    srv = subprocess.Popen(
        [str(calc_server), "--transport=tcp", f"--port={port}"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        deadline = time.monotonic() + 5
        connected = False
        while time.monotonic() < deadline:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
                probe.settimeout(0.2)
                try:
                    probe.connect(("127.0.0.1", port))
                    connected = True
                    break
                except OSError:
                    time.sleep(0.1)
        if not connected:
            results.report("rmi-roundtrip", "FAIL", "calc-server never started listening")
            return

        script = 'c = instantiate("", "Calculator")\nc.call("add", {"a": 2, "b": 5})\nexit\n'
        cli_run = subprocess.run(
            [str(cli), "--transport=tcp", f"--port={port}"],
            input=script, capture_output=True, text=True, timeout=10,
        )
        if cli_run.returncode != 0:
            results.report("rmi-roundtrip", "FAIL", f"bison-cli exited {cli_run.returncode}: {cli_run.stderr.strip()[:300]}")
            return
        if "7" not in cli_run.stdout:
            results.report("rmi-roundtrip", "FAIL", f"unexpected output: {cli_run.stdout.strip()[:300]}")
            return
        results.report("rmi-roundtrip", "PASS", "instantiated Calculator over TCP, add(2, 5) == 7")
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=5)
        except subprocess.TimeoutExpired:
            srv.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("zip_path", type=Path, help="Path to the release zip, e.g. dist/bison-1.2.3-Linux-x86_64.zip")
    parser.add_argument("--calc-server", type=Path, default=None,
                         help="Path to a calc-server binary built from this tree (e.g. build-release/calc-server) "
                              "to run a live RMI round trip. Skipped if omitted.")
    args = parser.parse_args()

    if not args.zip_path.exists():
        print(f"error: {args.zip_path} does not exist", file=sys.stderr)
        return 1

    results = Results()
    with tempfile.TemporaryDirectory(prefix="bison-validate-") as td:
        root = Path(td)
        with zipfile.ZipFile(args.zip_path) as zf:
            zf.extractall(root)
            if platform.system() != "Windows":
                for info in zf.infolist():
                    mode = (info.external_attr >> 16) & 0o777
                    if mode:
                        (root / info.filename).chmod(mode)

        check_structure(root, results)
        check_permissions(root, results)
        check_cli_smoke(root, results)
        check_dynamic_link(root, results)
        check_abi_link(root, results)
        check_csharp_dll(root, results)
        if args.calc_server:
            check_rmi_roundtrip(root, args.calc_server.resolve(), results)
        else:
            results.report("rmi-roundtrip", "SKIP", "pass --calc-server PATH to enable")

    print()
    passed = sum(1 for _, s, _ in results.entries if s == "PASS")
    failed = sum(1 for _, s, _ in results.entries if s == "FAIL")
    skipped = sum(1 for _, s, _ in results.entries if s == "SKIP")
    print(f"{passed} passed, {failed} failed, {skipped} skipped")
    if not results.ok:
        print("\nNOT ready to publish -- fix the failures above.")
        return 1
    print("\nLooks good to publish.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
