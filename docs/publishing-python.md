# Publishing the Python bindings to PyPI

This document is the release runbook for the **`bison-abi`** package
(`bindings/python/`). It covers the one-time account/infrastructure setup
and the per-release steps.

For *using* the bindings, see
[docs/bindings.md](bindings.md#python-bindingspython). For the build itself,
see [`bindings/python/pyproject.toml`](../bindings/python/pyproject.toml)
(the `[tool.cibuildwheel]` section) and
[`.github/workflows/release-pypi.yml`](../.github/workflows/release-pypi.yml).

## How it works

`pip install bison-abi` downloads a **pre-built wheel** matching the user's
OS / architecture / Python. pip only falls back to compiling from an sdist
when no wheel matches — and this package publishes **no sdist** (its
`cmake.source-dir` points at the repo root, outside the package directory),
so an unsupported platform gets a clean
`ERROR: No matching distribution` rather than a broken source build.

Wheels are produced by [cibuildwheel](https://cibuildwheel.pypa.io/) in
`release-pypi.yml`, one per platform:

| Platform | Runner | Notes |
|---|---|---|
| Linux x86_64 (`manylinux_2_28`) | `ubuntu-latest` | glibc ≥ 2.28 (distros from 2018+) |
| Linux aarch64 (`manylinux_2_28`) | `ubuntu-24.04-arm` | native, no QEMU |
| macOS arm64 + x86_64 | `macos-14` | one job cross-builds both; only the native arch is tested |
| Windows amd64 | `windows-latest` | `delvewheel` vendors runtime DLLs |

`wheel.py-api = "py3"` in `pyproject.toml` means one wheel per platform
serves every CPython ≥ 3.8, so the matrix builds against a single
interpreter (`cp312`). `bison_abi` is a plain ctypes-loaded shared library,
not a CPython extension, so no interpreter-specific tagging is needed.

The vendored C dependencies (libuv, mbedTLS) are linked **statically** into
`libbison_abi` — the root `CMakeLists.txt` forces `LIBUV_BUILD_SHARED` and
`USE_SHARED_MBEDTLS_LIBRARY` off. The wheel therefore ships just that one
shared object plus the `bison` Python package, and needs **no system
runtime libraries** — there is no `before-all` step and no runtime caveat
for the consumer. `auditwheel` / `delocate` / `delvewheel` bundle anything
the repair step still finds.

## One-time setup

### 1. PyPI accounts

1. Register at [pypi.org](https://pypi.org/account/register/) and enable
   2FA (PyPI refuses uploads without it). Save the recovery codes.
2. Register separately at [test.pypi.org](https://test.pypi.org/account/register/)
   — a distinct database with distinct credentials — for release
   rehearsals. Enable 2FA there too.

No API tokens are needed: publishing uses **Trusted Publishing** (OIDC),
so there are no secrets stored in the repo.

### 2. `bison-abi` is the base dependency

`bison-abi` has no runtime dependencies of its own, but
`wish-abi` (`binary-dice-games/wish`) declares
`dependencies = ["bison-abi>=1.0.0"]`. pip must be able to resolve that
from PyPI or the very first `wish-abi` install fails — so run this entire
runbook for `bison-abi` **before** doing the equivalent for `wish-abi`.

### 3. Register the Trusted Publisher on PyPI

Because the project does not exist yet, use the **pending publisher** flow
(it creates the project on first upload):

1. Go to [pypi.org/manage/account/publishing](https://pypi.org/manage/account/publishing/).
2. Under **"Add a new pending publisher"**, enter exactly:

   | Field | Value |
   |---|---|
   | PyPI Project Name | `bison-abi` |
   | Owner | `binary-dice-games` |
   | Repository name | `bison` |
   | Workflow name | `release-pypi.yml` |
   | Environment name | `pypi` |

3. Repeat on [test.pypi.org/manage/account/publishing](https://test.pypi.org/manage/account/publishing/)
   with the same values, for the rehearsal below.

"Workflow name" is the filename only, not a path. "Environment name" must
match `environment: pypi` in the workflow's `publish` job.

### 4. Create the GitHub Environment

1. Repo **Settings → Environments → New environment**, name it `pypi`
   (exact match).
2. Recommended protection rules:
   - **Deployment branches and tags** → restrict to tag pattern `py-v*`,
     so the OIDC token is only issuable from a release tag.
   - **Required reviewers** → add a maintainer, so each publish pauses for
     a manual approval click.
3. No environment secrets or variables are needed.

## Rehearse on TestPyPI

Temporarily point the publish step at TestPyPI:

```yaml
      - uses: pypa/gh-action-pypi-publish@release/v1
        with:
          repository-url: https://test.pypi.org/legacy/
```

Also loosen the `publish` job's `if:` so it runs from a branch, push a
pre-release tag such as `py-v0.0.1rc1`, watch **Actions**, then revert the
edit.

Verify the upload in a throwaway container:

```bash
docker run --rm -it python:3.12-slim bash -c '
  pip install --index-url https://test.pypi.org/simple/ bison-abi &&
  python -c "import bison, bison.dynamic, bison.rmi; print(bison.__file__)"'
```

## Each release

1. Bump `project.version` in
   [`bindings/python/pyproject.toml`](../bindings/python/pyproject.toml).
   The wheel version is read from that file; the git tag only starts the
   run. **Version numbers are permanently burned on PyPI** — a broken
   `1.0.0` upload cannot be replaced, only superseded by `1.0.1`.
2. Commit the bump to `main`.
3. *(Optional pre-flight)* **Actions → Build and publish Python wheels →
   Run workflow** on `main`. The `build-wheels` matrix runs; `publish` is
   skipped (its `if:` needs a `py-v` tag). Confirms the build is green on
   all four runners before you tag.
4. Tag and push:
   ```bash
   git tag py-v1.0.0
   git push origin py-v1.0.0
   ```
5. Watch **Actions**. Approve the `publish` deployment if you configured
   required reviewers.
6. `bison-abi` is now on PyPI.

## Verify

```bash
docker run --rm -it python:3.12-slim bash -c '
  pip install bison-abi &&
  python -c "import bison, bison.dynamic, bison.rmi; print(\"ok\")"'
```

## Build wheels locally

To reproduce a matrix job on your own machine:

```bash
git submodule update --init --recursive
pipx run cibuildwheel --output-dir wheelhouse bindings/python
```

cibuildwheel must run from the repo root (this command does), because
`cmake.source-dir = "../.."` needs the whole tree — including checked-out
submodules — in the build context.

## Gotchas

- **`ubuntu-24.04-arm`** runners are free for public repos. If `bison`
  becomes private and the org has not enabled arm64 runners, that matrix
  entry queues forever — drop it and add QEMU (`docker/setup-qemu-action`
  + `CIBW_ARCHS_LINUX: auto aarch64`) instead.
- **Name normalization**: PyPI treats `bison-abi`, `bison_abi`, and
  `Bison.ABI` as the same project.
- **First upload creates the project** — there is no separate
  "reserve the name" step with pending publishers.
- The `publish` job's `permissions: id-token: write` and
  `environment: pypi` are what make OIDC work. Do not remove them.
- If the Linux build fails on a C++20 library feature (`std::bit_cast`,
  etc.), check that the `manylinux_2_28` image override in
  `[tool.cibuildwheel.linux]` is still present — the default
  `manylinux2014` image's GCC 10 toolchain is too old.
- If the **macOS x86_64** build fails with `'path' is unavailable:
  introduced in macOS 10.15` (or similar `std::filesystem` errors), check
  that `MACOSX_DEPLOYMENT_TARGET = "10.15"` is still set in
  `[tool.cibuildwheel.macos]`. cibuildwheel's default for the Intel build
  is 10.13, below `std::filesystem`'s libc++ availability floor. The arm64
  job won't catch a regression here — it's always ≥ 11.0, and `macos-14`
  only tests the native arch.
