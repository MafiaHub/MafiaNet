# Design: vcpkg support for MafiaNet

**Date:** 2026-05-31
**Status:** Approved (pending implementation)

## 1. Goal

Make MafiaNet consumable via [vcpkg](https://github.com/microsoft/vcpkg) by:

1. Adding an **in-repo overlay port** (`portfile.cmake` + `vcpkg.json`).
2. Adding the **git-registry plumbing** (`versions/`) so downstream projects can
   pull MafiaNet directly from the `MafiaHub/MafiaNet` GitHub repo as a vcpkg
   registry — no upstream `microsoft/vcpkg` submission required.

The port builds the **library only** (no samples/tests), honors the triplet's
linkage selection, and exposes a single stable `MafiaNet::MafiaNet` target
regardless of static/dynamic linkage.

## 2. Background / current state

- MafiaNet already ships a proper CMake install/export:
  - Config installed to `lib/cmake/MafiaNet` (`MafiaNetConfig.cmake` generated
    from `cmake/MafiaNetConfig.cmake.in`).
  - Export namespace `MafiaNet::`, export file `MafiaNetTargets.cmake`.
  - `find_dependency(OpenSSL)` + `find_dependency(Threads)` in the config.
- The build exposes **two differently-named targets** depending on options:
  - shared build → `MafiaNet::MafiaNet` (`MAFIANET_BUILD_SHARED`, default ON)
  - static build → `MafiaNet::MafiaNetStatic` (`MAFIANET_BUILD_STATIC`, default ON)
- Sample-only dependencies (bzip2, miniupnpc, opus, rnnoise) are fetched via
  `FetchContent` only when `MAFIANET_BUILD_SAMPLES=ON`. **Out of scope** for the port.
- The only external runtime dependency of the library is **OpenSSL 3.0+**.
- License: **MIT** (`LICENSE.md`).
- Release tag `v0.4.0` exists on `MafiaHub/MafiaNet`
  (commit `f56f22e86cb7b1b05201c4925383bb8e2e8953bb`).

## 3. The key decision: stable target across linkages (Approach A)

vcpkg selects static-vs-dynamic via the **triplet**, and an idiomatic port builds
only that one linkage. Because MafiaNet exports two differently-named targets,
downstream code would otherwise need a different `target_link_libraries` line per
triplet — bad UX.

**Chosen approach (A):** the port honors `VCPKG_LIBRARY_LINKAGE` (builds one
linkage), and `cmake/MafiaNetConfig.cmake.in` gains a small backward-compatible
block so `MafiaNet::MafiaNet` always resolves to whichever linkage was installed.

Rejected: (B) building both linkages — wasteful and non-idiomatic; (C) documenting
both target names — leaks linkage into consumer CMake.

## 4. New files / layout

```
ports/mafianet/
  vcpkg.json          # port manifest
  portfile.cmake      # build recipe
  usage               # printed after install
versions/
  baseline.json       # registry baseline: mafianet -> 0.4.0, port-version 0
  m-/mafianet.json    # version -> git-tree mapping
docs/examples/vcpkg/
  vcpkg-configuration.json   # consumer references this repo as a registry
  vcpkg.json                 # consumer manifest depending on mafianet
  CMakeLists.txt             # minimal consumer example
```

Plus an edit to `cmake/MafiaNetConfig.cmake.in` (section 3) and a new
"Installing via vcpkg" section in `README.md`.

## 5. `ports/mafianet/vcpkg.json`

- `name`: `mafianet`
- `version-semver`: `0.4.0`
- `description`, `homepage` (`https://github.com/mafiahub/MafiaNet`)
- `license`: `MIT`
- `dependencies`:
  - `openssl`
  - `{ "name": "vcpkg-cmake", "host": true }`
  - `{ "name": "vcpkg-cmake-config", "host": true }`
- No features. Samples/voice/autopatcher deps are intentionally excluded (YAGNI).

## 6. `ports/mafianet/portfile.cmake`

```cmake
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MafiaHub/MafiaNet
    REF "v${VERSION}"
    SHA512 9fe6fe79149f9e77a3cbaf9a9dcd7f87ebaf05f396021d703ebd60a35356ce698fc6009aeac0fad9f93194b12943f4c9bd5996f47fd94148f830aa7aa6246baf
    HEAD_REF master
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static"  MAFIANET_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" MAFIANET_SHARED)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMAFIANET_BUILD_STATIC=${MAFIANET_STATIC}
        -DMAFIANET_BUILD_SHARED=${MAFIANET_SHARED}
        -DMAFIANET_BUILD_SAMPLES=OFF
        -DMAFIANET_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME MafiaNet CONFIG_PATH lib/cmake/MafiaNet)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
```

Notes:
- The `SHA512` is the value computed from the `v0.4.0` GitHub source tarball on
  2026-05-31. GitHub auto-generated tarball hashes have shifted historically; if
  vcpkg reports a mismatch on first build, update the recipe with vcpkg's reported
  "Actual hash".
- OpenSSL is a `PUBLIC` link dependency, so the installed `MafiaNetConfig.cmake`'s
  `find_dependency(OpenSSL)` resolves it from vcpkg's installed tree.

## 7. Stable-target edit to `cmake/MafiaNetConfig.cmake.in`

After `include(".../MafiaNetTargets.cmake")` and before
`check_required_components`, add:

```cmake
# Provide a stable consumption target regardless of linkage (e.g. vcpkg static
# triplets only install MafiaNet::MafiaNetStatic). Backward-compatible: a normal
# shared install already defines MafiaNet::MafiaNet, so this block is skipped.
if(NOT TARGET MafiaNet::MafiaNet AND TARGET MafiaNet::MafiaNetStatic)
    add_library(MafiaNet::MafiaNet INTERFACE IMPORTED)
    set_target_properties(MafiaNet::MafiaNet PROPERTIES
        INTERFACE_LINK_LIBRARIES MafiaNet::MafiaNetStatic)
endif()
```

## 8. Registry plumbing

- `versions/m-/mafianet.json` maps version `0.4.0` to the **git-tree hash** of
  `ports/mafianet` (`git rev-parse HEAD:ports/mafianet`).
- `versions/baseline.json` records `mafianet → { "baseline": "0.4.0", "port-version": 0 }`.
- Generated/validated with `vcpkg x-add-version mafianet` once the port files are
  committed. If the `vcpkg` CLI is unavailable on PATH, compute the git-tree hash
  manually and hand-write the two JSON files in the exact vcpkg schema.

## 9. Consumer documentation (`README.md` + `docs/examples/vcpkg/`)

A new "Installing via vcpkg" section documenting two consumption paths:

**Overlay ports** (quick / local):
```
vcpkg install mafianet --overlay-ports=<path>/MafiaNet/ports
# or set VCPKG_OVERLAY_PORTS
```

**Git registry** (recommended for downstream projects) — `vcpkg-configuration.json`:
```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/MafiaHub/MafiaNet",
      "baseline": "<commit-sha>",
      "packages": ["mafianet"]
    }
  ]
}
```
plus a `vcpkg.json` depending on `mafianet`, consumed via:
```cmake
find_package(MafiaNet CONFIG REQUIRED)
target_link_libraries(app PRIVATE MafiaNet::MafiaNet)
```

## 10. Verification

- `vcpkg install mafianet --overlay-ports=ports` for a **dynamic** and a **static**
  triplet (locally if `vcpkg` is available; otherwise documented as the manual
  acceptance step).
- Configure + build the `docs/examples/vcpkg` consumer against the installed port
  and confirm `MafiaNet::MafiaNet` links under both linkages.
- `vcpkg x-add-version --verify` for registry correctness.

## 11. Out of scope

- Sample/extension dependencies and a `samples` feature.
- Submitting the port to the upstream `microsoft/vcpkg` registry.
- Changes to MafiaNet's library build beyond the config-template alias.
