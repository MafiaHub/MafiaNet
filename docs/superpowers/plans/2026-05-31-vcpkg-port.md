# vcpkg Port Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MafiaNet installable via vcpkg as an in-repo overlay port that doubles as a git registry, exposing a stable `MafiaNet::MafiaNet` target under any linkage.

**Architecture:** Add `ports/mafianet/` (port manifest + recipe) that builds from the `v0.4.0` GitHub tag, honoring `VCPKG_LIBRARY_LINKAGE`. Add a backward-compatible alias block to the installed CMake config so the consumption target name is stable. Add `versions/` registry plumbing and consumer-facing docs/examples.

**Tech Stack:** vcpkg (manifest + classic/overlay + git registry), CMake 3.21+, OpenSSL 3.0+, GitHub source tarballs.

**Environment note:** `vcpkg` is **not** installed in the dev environment for this plan. Steps validate what is checkable with `python3`/`cmake`/`git`; the full `vcpkg install` round-trip is an explicit **manual acceptance step** (Task 8) for a machine that has vcpkg.

**Branch:** `feat/vcpkg-port` (already created; the design spec is committed there at `docs/superpowers/specs/2026-05-31-vcpkg-port-design.md`).

---

## File Structure

Files created/modified, each with one responsibility:

- `cmake/MafiaNetConfig.cmake.in` — **modify**: add stable-target alias block.
- `ports/mafianet/vcpkg.json` — **create**: port manifest (name, version, deps).
- `ports/mafianet/portfile.cmake` — **create**: build recipe (fetch, configure, install, fixup).
- `ports/mafianet/usage` — **create**: post-install usage hint.
- `versions/baseline.json` — **create**: registry baseline.
- `versions/m-/mafianet.json` — **create**: version → git-tree mapping.
- `docs/examples/vcpkg/vcpkg-configuration.json` — **create**: consumer registry config example.
- `docs/examples/vcpkg/vcpkg.json` — **create**: consumer manifest example.
- `docs/examples/vcpkg/CMakeLists.txt` — **create**: minimal consumer CMake.
- `README.md` — **modify**: add "Installing via vcpkg" section.

---

## Task 1: Stable consumption target in the installed config

Makes `MafiaNet::MafiaNet` resolve even when only the static target is installed (vcpkg static triplets). Backward-compatible: shared installs already define it, so the block is a no-op there.

**Files:**
- Modify: `cmake/MafiaNetConfig.cmake.in`
- Test: `/tmp/mn-alias-test.cmake` (throwaway, not committed)

- [ ] **Step 1: Write the failing test**

Create `/tmp/mn-alias-test.cmake` with exactly:

```cmake
# Simulate a vcpkg static-triplet install: only the static target exists.
add_library(MafiaNet::MafiaNetStatic STATIC IMPORTED)

# --- BEGIN: paste of the block to be added to MafiaNetConfig.cmake.in ---
# (left empty on purpose for the red run)
# --- END ---

if(NOT TARGET MafiaNet::MafiaNet)
    message(FATAL_ERROR "FAIL: MafiaNet::MafiaNet is not defined for a static-only install")
endif()
get_target_property(_linked MafiaNet::MafiaNet INTERFACE_LINK_LIBRARIES)
if(NOT _linked STREQUAL "MafiaNet::MafiaNetStatic")
    message(FATAL_ERROR "FAIL: MafiaNet::MafiaNet does not forward to the static target (got '${_linked}')")
endif()
message(STATUS "PASS: stable target resolves to the static library")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -P /tmp/mn-alias-test.cmake`
Expected: FAIL — `FAIL: MafiaNet::MafiaNet is not defined for a static-only install`

- [ ] **Step 3: Implement — add the alias block to the config template**

In `cmake/MafiaNetConfig.cmake.in`, insert the block between the
`include(".../MafiaNetTargets.cmake")` line and `check_required_components(MafiaNet)`.
The file should read:

```cmake
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

find_dependency(OpenSSL)
find_dependency(Threads)

include("${CMAKE_CURRENT_LIST_DIR}/MafiaNetTargets.cmake")

# Provide a stable consumption target regardless of linkage. vcpkg static
# triplets install only MafiaNet::MafiaNetStatic; downstream code should still be
# able to link MafiaNet::MafiaNet. A normal shared install already defines that
# target, so this block is skipped there (fully backward-compatible).
if(NOT TARGET MafiaNet::MafiaNet AND TARGET MafiaNet::MafiaNetStatic)
    add_library(MafiaNet::MafiaNet INTERFACE IMPORTED)
    set_target_properties(MafiaNet::MafiaNet PROPERTIES
        INTERFACE_LINK_LIBRARIES MafiaNet::MafiaNetStatic)
endif()

check_required_components(MafiaNet)
```

- [ ] **Step 4: Update the test with the real block and verify it passes**

Replace the `--- BEGIN/END ---` region in `/tmp/mn-alias-test.cmake` with the exact
block added in Step 3:

```cmake
# --- BEGIN: paste of the block from MafiaNetConfig.cmake.in ---
if(NOT TARGET MafiaNet::MafiaNet AND TARGET MafiaNet::MafiaNetStatic)
    add_library(MafiaNet::MafiaNet INTERFACE IMPORTED)
    set_target_properties(MafiaNet::MafiaNet PROPERTIES
        INTERFACE_LINK_LIBRARIES MafiaNet::MafiaNetStatic)
endif()
# --- END ---
```

Run: `cmake -P /tmp/mn-alias-test.cmake`
Expected: PASS — `PASS: stable target resolves to the static library`

- [ ] **Step 5: Commit**

```bash
git add cmake/MafiaNetConfig.cmake.in
git commit -m "feat(cmake): expose stable MafiaNet::MafiaNet target for static-only installs"
```

---

## Task 2: Port manifest (`ports/mafianet/vcpkg.json`)

**Files:**
- Create: `ports/mafianet/vcpkg.json`

- [ ] **Step 1: Write the file**

```json
{
  "name": "mafianet",
  "version-semver": "0.4.0",
  "description": "Cross-platform network engine for multiplayer games (RakNet/SLikeNet fork).",
  "homepage": "https://github.com/mafiahub/MafiaNet",
  "license": "MIT",
  "dependencies": [
    "openssl",
    {
      "name": "vcpkg-cmake",
      "host": true
    },
    {
      "name": "vcpkg-cmake-config",
      "host": true
    }
  ]
}
```

- [ ] **Step 2: Validate JSON**

Run: `python3 -m json.tool ports/mafianet/vcpkg.json > /dev/null && echo VALID`
Expected: `VALID`

- [ ] **Step 3: Verify version matches the project**

Run: `grep -n 'VERSION 0.4.0' CMakeLists.txt`
Expected: a line `project(MafiaNet` block showing `VERSION 0.4.0` (the port version must equal the canonical project version).

- [ ] **Step 4: Commit**

```bash
git add ports/mafianet/vcpkg.json
git commit -m "feat(vcpkg): add mafianet port manifest"
```

---

## Task 3: Port recipe (`ports/mafianet/portfile.cmake`)

**Files:**
- Create: `ports/mafianet/portfile.cmake`

- [ ] **Step 1: Write the file**

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

- [ ] **Step 2: Structural sanity checks**

Run:
```bash
grep -q 'REPO MafiaHub/MafiaNet'        ports/mafianet/portfile.cmake && \
grep -q 'REF "v${VERSION}"'             ports/mafianet/portfile.cmake && \
grep -q 'vcpkg_cmake_config_fixup'      ports/mafianet/portfile.cmake && \
grep -q 'PACKAGE_NAME MafiaNet'         ports/mafianet/portfile.cmake && \
grep -q 'CONFIG_PATH lib/cmake/MafiaNet' ports/mafianet/portfile.cmake && \
echo OK
```
Expected: `OK`

- [ ] **Step 3: Confirm the SHA512 still matches the published tarball**

Run:
```bash
curl -sL https://github.com/MafiaHub/MafiaNet/archive/refs/tags/v0.4.0.tar.gz | shasum -a 512
```
Expected: begins with `9fe6fe79149f9e77a3cbaf9a9dcd7f87ebaf05f396021d703ebd60a35356ce698`.
If it differs (GitHub re-compression), update the `SHA512` line in the portfile to the printed value before committing.

- [ ] **Step 4: Commit**

```bash
git add ports/mafianet/portfile.cmake
git commit -m "feat(vcpkg): add mafianet portfile recipe"
```

---

## Task 4: Usage hint (`ports/mafianet/usage`)

**Files:**
- Create: `ports/mafianet/usage`

- [ ] **Step 1: Write the file**

```
mafianet provides CMake targets:

    find_package(MafiaNet CONFIG REQUIRED)
    target_link_libraries(main PRIVATE MafiaNet::MafiaNet)
```

- [ ] **Step 2: Verify content**

Run: `grep -q 'find_package(MafiaNet CONFIG REQUIRED)' ports/mafianet/usage && grep -q 'MafiaNet::MafiaNet' ports/mafianet/usage && echo OK`
Expected: `OK`

- [ ] **Step 3: Commit**

```bash
git add ports/mafianet/usage
git commit -m "feat(vcpkg): add mafianet usage hint"
```

---

## Task 5: Registry plumbing (`versions/`)

The version entry references the **git-tree hash** of `ports/mafianet`, which only
exists after the port files (Tasks 2–4) are committed. This task therefore runs
after those commits.

**Files:**
- Create: `versions/baseline.json`
- Create: `versions/m-/mafianet.json`

- [ ] **Step 1: Compute the git-tree hash of the port directory**

Run: `git rev-parse HEAD:ports/mafianet`
Expected: a 40-char hex SHA (the tree object for `ports/mafianet` at HEAD). Copy it; call it `<TREE_SHA>` below.

- [ ] **Step 2: Write `versions/baseline.json`**

```json
{
  "default": {
    "mafianet": {
      "baseline": "0.4.0",
      "port-version": 0
    }
  }
}
```

- [ ] **Step 3: Write `versions/m-/mafianet.json`**

Replace `<TREE_SHA>` with the value from Step 1:

```json
{
  "versions": [
    {
      "version-semver": "0.4.0",
      "git-tree": "<TREE_SHA>",
      "port-version": 0
    }
  ]
}
```

- [ ] **Step 4: Validate JSON and the git-tree reference**

Run:
```bash
python3 -m json.tool versions/baseline.json > /dev/null && \
python3 -m json.tool versions/m-/mafianet.json > /dev/null && \
TREE=$(python3 -c "import json;print(json.load(open('versions/m-/mafianet.json'))['versions'][0]['git-tree'])") && \
git cat-file -t "$TREE" && echo OK
```
Expected: prints `tree` then `OK` (confirms the referenced object exists and is a tree).

- [ ] **Step 5: Commit**

```bash
git add versions/baseline.json versions/m-/mafianet.json
git commit -m "feat(vcpkg): add git-registry version metadata for mafianet 0.4.0"
```

> Note: if the port files are later amended (changing their tree), re-run Step 1
> and update `git-tree` in `versions/m-/mafianet.json`. On a machine with vcpkg,
> `vcpkg x-add-version mafianet --verify` automates and checks this.

---

## Task 6: Consumer example — registry config + manifest

**Files:**
- Create: `docs/examples/vcpkg/vcpkg-configuration.json`
- Create: `docs/examples/vcpkg/vcpkg.json`

- [ ] **Step 1: Write `docs/examples/vcpkg/vcpkg-configuration.json`**

The `baseline` is the commit SHA of MafiaNet that contains the port + version files;
`<MAFIANET_COMMIT>` is a placeholder the consumer fills with a real commit (e.g. the
tip of `feat/vcpkg-port` once merged, or the `v0.4.0`-and-later commit that adds
`versions/`).

```json
{
  "default-registry": {
    "kind": "git",
    "repository": "https://github.com/microsoft/vcpkg",
    "baseline": "<VCPKG_BASELINE_COMMIT>"
  },
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/MafiaHub/MafiaNet",
      "baseline": "<MAFIANET_COMMIT>",
      "packages": [
        "mafianet"
      ]
    }
  ]
}
```

- [ ] **Step 2: Write `docs/examples/vcpkg/vcpkg.json`**

```json
{
  "name": "mafianet-consumer-example",
  "version": "0.0.1",
  "dependencies": [
    "mafianet"
  ]
}
```

- [ ] **Step 3: Validate JSON**

Run:
```bash
python3 -m json.tool docs/examples/vcpkg/vcpkg-configuration.json > /dev/null && \
python3 -m json.tool docs/examples/vcpkg/vcpkg.json > /dev/null && echo VALID
```
Expected: `VALID`

- [ ] **Step 4: Commit**

```bash
git add docs/examples/vcpkg/vcpkg-configuration.json docs/examples/vcpkg/vcpkg.json
git commit -m "docs(vcpkg): add consumer registry + manifest example"
```

---

## Task 7: Consumer example — minimal CMake project

**Files:**
- Create: `docs/examples/vcpkg/CMakeLists.txt`

- [ ] **Step 1: Write the file**

```cmake
cmake_minimum_required(VERSION 3.21)
project(mafianet_consumer_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Consumed from vcpkg (manifest mode). The same find_package call works whether
# the active triplet is static or dynamic, thanks to the stable alias target.
find_package(MafiaNet CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE MafiaNet::MafiaNet)
```

- [ ] **Step 2: Verify content**

Run: `grep -q 'find_package(MafiaNet CONFIG REQUIRED)' docs/examples/vcpkg/CMakeLists.txt && grep -q 'MafiaNet::MafiaNet' docs/examples/vcpkg/CMakeLists.txt && echo OK`
Expected: `OK`

- [ ] **Step 3: Commit**

```bash
git add docs/examples/vcpkg/CMakeLists.txt
git commit -m "docs(vcpkg): add minimal consumer CMakeLists example"
```

---

## Task 8: README "Installing via vcpkg" section + manual acceptance

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Locate the insertion point**

Run: `grep -n '^## ' README.md | head -40`
Expected: a list of top-level sections. Insert the new section after the existing
build/installation section and before the Changelog (pick the nearest install-related
heading from the output).

- [ ] **Step 2: Add the section**

Insert this Markdown (adjust the surrounding blank lines to match the file):

````markdown
## Installing via vcpkg

MafiaNet ships an in-repo vcpkg port. There are two ways to consume it.

### Option A — Overlay port (quickest)

Point vcpkg at this repo's `ports/` directory:

```bash
vcpkg install mafianet --overlay-ports=/path/to/MafiaNet/ports
# or: export VCPKG_OVERLAY_PORTS=/path/to/MafiaNet/ports
```

### Option B — Git registry (recommended for projects)

Add a `vcpkg-configuration.json` next to your `vcpkg.json` (see
`docs/examples/vcpkg/`):

```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/MafiaHub/MafiaNet",
      "baseline": "<commit-sha-containing-the-port>",
      "packages": ["mafianet"]
    }
  ]
}
```

Then depend on it in `vcpkg.json`:

```json
{ "name": "my-app", "version": "0.0.1", "dependencies": ["mafianet"] }
```

### Using it from CMake

The same call works for static and dynamic triplets:

```cmake
find_package(MafiaNet CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE MafiaNet::MafiaNet)
```
````

- [ ] **Step 3: Verify the section landed**

Run: `grep -q '## Installing via vcpkg' README.md && grep -q 'overlay-ports' README.md && echo OK`
Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add README.md
git commit -m "docs(vcpkg): document installing MafiaNet via vcpkg"
```

- [ ] **Step 5: Manual acceptance on a vcpkg-equipped machine (record results)**

These cannot run in the current environment (no `vcpkg`). Run on a machine with vcpkg
and note pass/fail:

```bash
# Dynamic linkage
vcpkg install mafianet --overlay-ports=ports --triplet x64-linux-dynamic
# Static linkage
vcpkg install mafianet --overlay-ports=ports --triplet x64-linux

# Build the example consumer against the installed port
cmake -S docs/examples/vcpkg -B /tmp/mn-consumer \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_OVERLAY_PORTS="$PWD/ports"
cmake --build /tmp/mn-consumer
```
Expected: both installs succeed; the consumer configures and links `MafiaNet::MafiaNet`
under both triplets. (The example needs a trivial `main.cpp`; create one that calls
`MafiaNet::RakPeerInterface::GetInstance()` and returns 0.)

If vcpkg is available, also run `vcpkg x-add-version mafianet --verify` to confirm the
`versions/` metadata is consistent, and update `git-tree` if it reports a diff.

---

## Self-Review notes

- **Spec coverage:** §3 alias → Task 1; §5 manifest → Task 2; §6 portfile → Task 3;
  usage (§4 layout) → Task 4; §8 registry → Task 5; §9 consumer docs → Tasks 6–7 +
  README in Task 8; §10 verification → Task 8 Step 5. All spec sections mapped.
- **Ordering:** Task 5 (versions) intentionally follows the port commits so the
  git-tree hash exists.
- **No vcpkg locally:** the real install round-trip is isolated to Task 8 Step 5 and
  clearly marked manual; everything else is verifiable with `python3`/`cmake`/`git`.
