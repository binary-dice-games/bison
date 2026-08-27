# Publishing the bison binaries to a GitHub Release

This document is the release runbook for the **downloadable binary zips**
attached to each GitHub Release (`bison-<version>-<System>-<arch>.zip`). It
covers what the automation does and the per-release steps.

For the packaging mechanics (what goes in the zip, how to build one
locally, PATH-setup scripts), see
[docs/building.md](building.md#packaging-a-release). For the `bison-abi`
PyPI package, see [docs/publishing-python.md](publishing-python.md).

## How it works

[`.github/workflows/release-binaries.yml`](../.github/workflows/release-binaries.yml)
fires on `release: published`. Its matrix:

| Runner | Toolchain | Asset |
|---|---|---|
| `ubuntu-latest` | Ninja + GCC | `bison-<ver>-Linux-x86_64.zip` |
| `ubuntu-24.04-arm` | Ninja + GCC | `bison-<ver>-Linux-aarch64.zip` |
| `macos-14` | Ninja + AppleClang | `bison-<ver>-Darwin-arm64.zip` |
| `windows-latest` | Ninja + MSVC (`cl.exe` via `ilammy/msvc-dev-cmd`) | `bison-<ver>-Windows-AMD64.zip` |

Each job runs
[`scripts/package_release.py`](../scripts/package_release.py), then
[`scripts/validate_release.py`](../scripts/validate_release.py) on the
resulting zip (structure, `bison-cli --version`, dynamic-link, and a
compile-and-run check of the packaged `bison_c.h` against the packaged
shared lib), then `gh release upload <tag> dist/*.zip --clobber` attaches
it to the release. `--clobber` makes re-runs idempotent.

The zip version comes from the git tag with the leading `v` stripped
(`v1.2.3` -> `1.2.3`), passed as `package_release.py --version` (which
feeds `-DBISON_PACKAGE_VERSION`).

### Platform notes

- **Windows** zips are MSVC (`cl.exe`) builds via the Ninja generator. The
  build steps run under `bash`, which strips the `ProgramFiles(x86)` env
  var that CMake's "Visual Studio" generator relies on to find a VS
  instance, so the workflow sets up the compiler explicitly with
  `ilammy/msvc-dev-cmd` and builds with Ninja instead. libuv and mbedTLS
  are linked statically, so the only runtime requirement is the
  [Microsoft Visual C++ redistributable](https://aka.ms/vs/17/release/vc_redist.x64.exe).
  A self-contained mingw build can still be produced by hand
  (`package_release.py` from an MSYS2 shell bundles the mingw runtime DLLs
  via `ldd`).
- **macOS** builds arm64 only (`macos-14`). Intel Macs build from source.
- **C# binding DLL** is bundled only when the `dotnet` SDK is present on
  the runner (`setup-dotnet` is `continue-on-error`). If a job's log shows
  `dotnet not found ... skipping C# DLL bundle`, that zip ships the C#
  binding as source only.
- `examples/` binaries and `src/srv/calc`'s `calc-server` are deliberately
  excluded from the zip — demo/tutorial code, not something an ABI/CLI
  consumer needs.

## One-time setup

None. The workflow uses the automatic `GITHUB_TOKEN` (`permissions:
contents: write`) — no Trusted Publisher, environment, or secret.
`ubuntu-24.04-arm` runners are free for public repos; if the repo goes
private without org-enabled arm64 runners, that matrix entry queues
forever — drop it or swap in QEMU.

## Each release

1. Land everything for the release on `main`.
2. *(Optional pre-flight)* **Actions → Build and publish release binaries →
   Run workflow** on `main`. The full matrix builds, validates, and uploads
   the zips as workflow artifacts; nothing touches a release. Confirms all
   four runners are green before you tag.
3. Draft the release on GitHub (**Releases → Draft a new release**), create
   a new tag `v<version>` targeting `main`, write the notes.
4. **Publish** the release. The workflow starts.
5. Watch **Actions**. When it finishes, the release has four `.zip` assets.
   Re-running a failed job re-attaches its asset (`--clobber`).

## Verify

Download a zip from the release page and, on a clean machine of that
platform:

```sh
unzip bison-<version>-Linux-x86_64.zip -d bison && cd bison
source ./bison-env.sh          # . .\bison-env.ps1 on Windows
bison-cli --version
```

(This is the same set of checks `validate_release.py` already ran in CI.)

## Build a zip locally

```sh
git submodule update --init --recursive
python3 scripts/package_release.py --version <version>
python3 scripts/validate_release.py dist/bison-<version>-<System>-<arch>.zip
```

Run once per platform. See
[docs/building.md](building.md#packaging-a-release) for `--build-dir` /
`--generator` / `--skip-configure` details.

## Gotchas

- **Tag format**: must be `v<version>`. A tag without the `v` still builds,
  but the zip version string will include whatever prefix you used.
- **`Darwin` vs `macOS`**: the zip name uses `platform.system()`, which is
  `Darwin` on macOS — `bison-<version>-Darwin-arm64.zip`.
- **`AMD64` vs `x86_64`**: Windows `platform.machine()` returns `AMD64`;
  Linux returns `x86_64`. Expected, not a bug.
- **A missing `dotnet` SDK** on a runner silently downgrades that one zip
  to C#-source-only. Check each job's "Build and package" log if the C#
  DLL matters for a release.
- **`validate_release.py` `rmi-roundtrip` check** is skipped in CI — it
  needs `calc-server`, which is excluded from the zip. Run it locally with
  `--calc-server build-release/calc-server` for a full end-to-end check.
