# ANARI rendering in Blender on macOS

[WINDOWS.md](WINDOWS.md) describes the ANARI stack as it is built and tested on Windows
(`F:\work\anari`). This document covers the same stack on macOS: how the sources get to the
Mac, how each component is built there, what differs from Windows and what does not work.

Everything here was done on one machine: an Apple M4 (arm64, 10 cores, 16 GB), macOS 26.3,
Xcode with Apple clang 21, CMake 4.3.4 and Ninja from Homebrew.

Contents:

1. [State](#1-state)
2. [Workspace](#2-workspace)
3. [Synchronizing the sources](#3-synchronizing-the-sources)
4. [Building](#4-building)
5. [Running Blender and the smoke tests](#5-running-blender-and-the-smoke-tests)
6. [Differences from Windows](#6-differences-from-windows)
7. [What does not work](#7-what-does-not-work)
8. [Smoke test results](#8-smoke-test-results)
9. [pynari](#9-pynari)
10. [Source changes made for macOS](#10-source-changes-made-for-macos)

## 1. State

State of 2026-10-11.

| Component | ANARI library | macOS | Tested |
|---|---|---|---|
| ANARI-SDK 0.17 with helide | `helide` | built | smoke tests, same results as on Windows |
| Blender (`main-anari`) with the ANARI device | | built | runs the smoke tests |
| cyclesphi-anari, CPU | `cycles` | built | smoke tests, 16 of 16 pass; pynari 36 of 36 |
| cyclesphi-anari, Metal | `cycles` | built | smoke tests, 16 of 16 pass, same numbers as on the CPU; pynari 36 of 36 |
| barney (CPU back end) | `barney` | built | smoke tests, 16 of 16 pass; pynari 36 of 36 |
| visionaray, anari-visionaray (CPU) | `visionaray` | built | smoke tests, 16 of 16 pass; pynari 36 of 36 |
| anari-ospray with OSPRay 3.2.0 | `ospray` | built | smoke tests, 16 of 16 pass; pynari 36 of 36 |
| RadeonProRenderANARI, CPU | `rpr` | built, with the RPR SDK 3.1.6 | smoke tests, 16 of 16 pass; pynari 34 of 36 |
| RadeonProRenderANARI, GPU (Metal) | `rpr` | built, **renders wrong images**, see [section 7](#7-what-does-not-work) | smoke tests fail |
| Photon (Kokkos Threads, Embree) | `photon` | built | smoke tests, 16 of 16 pass; pynari 36 of 36 |
| mitsuba-anari, Metal (`metal_ad_rgb`) | `mitsuba` | built | smoke tests, 16 of 16 pass, same numbers as `cuda_ad_rgb` on Windows; pynari 36 of 36 |
| mitsuba-anari, LLVM (`llvm_ad_rgb`) | `mitsuba` | built | smoke tests, 15 of 16: the volume scene renders black, see [section 7](#7-what-does-not-work); pynari 36 of 36 |
| mitsuba-anari, `scalar_rgb` | `mitsuba` | built | the smoke suite crashes in its last test, see [section 7](#7-what-does-not-work) |
| pynari | | built | sample matrix, see [section 9](#9-pynari) |
| openmoonray-anari with MoonRay and moonshine (CPU) | `moonray` | built, on an APFS disk image ([4.10](#410-moonray-openmoonray-anari)) | smoke tests, 16 of 16 pass, same numbers as on Windows; pynari 36 of 36 |
| openmoonray-anari, XPU (Metal) | `moonray` | not built: it needs the Metal Toolchain component of Xcode, see [section 7](#7-what-does-not-work) | |
| VisRTX | `visrtx` | not possible: it needs CUDA and OptiX | |

There is no CUDA, OptiX or HIP on macOS, so the GPU configurations of WINDOWS.md
(`cycles` with OptiX, `visionaray_cuda`, Photon with Kokkos Cuda, `mitsuba` with
`cuda_ad_rgb`, barney with OptiX) do not exist here. The GPU of the Mac is used through
Metal: by cyclesphi-anari, by Mitsuba (`metal_ad_rgb`), and by Radeon ProRender (which does
not render correctly).

## 2. Workspace

The workspace is `~/scratch/anari`, with the directory names of the Windows workspace.
`~/scratch` is a symbolic link to an external disk; the scripts use the physical path of
the directory on that disk, because CMake resolves symbolic links.

```
~/scratch/anari/
  <repository>/     source mirrors (ANARI-SDK, blenderphi, cyclesphi-anari, barney, ...)
  blenderphi/lib/macos_arm64/   Blender's precompiled libraries (Git LFS)
  build/<name>/     build directories
  build/blender/bin/Blender.app
  build/logs/       build logs (<component>-configure|build|install.log), smoke-<name>.txt
  build/test_out/<name>/   images written by the smoke tests
  install/          lib/ (ANARI SDK, helide, barney, anari-visionaray, cyclesphi-anari +
                    lib/cycles), lib/cmake/anari-0.17.0, include/visionaray
  install/ospray/lib/   anari-ospray and the OSPRay runtime (Embree, Open VKL, OIDN, TBB)
  install/rpr/lib/      RadeonProRenderANARI, libRadeonProRender64, libNorthstar64, cache/
  install/mitsuba/lib/  mitsuba-anari, Mitsuba, Dr.Jit, plugins/, data/, libLLVM.dylib
  install/llvm-c/lib/   libLLVM.dylib with the LLVM C API only (for Mitsuba's LLVM variant)
  install/photon_cpu/lib/   Photon; install/kokkos-host: Kokkos
  install/moonray/      openmoonray-anari and the MoonRay runtime: lib/, bin/, rdl2dso/,
                        coredata/
  apfs.sparsebundle     an APFS disk image, mounted at /Volumes/anari-apfs: the build of
                        MoonRay (section 4.10); mnt is a link to the mounted volume
  build/pynari/         the pynari extension module; build/pynari-venv, build/pynari-out
  external/         third party releases (ospray, rpr-v3.1.6, kokkos)
  tools/            downloaded build tools (nothing needed so far)
  cache/            pip, XDG, matplotlib and Python caches; home/ and blender-user/ for
                    Blender runs; xdg/cache/cycles/kernels: the compiled Metal kernels
                    of Cycles; drjit: the kernel cache of Dr.Jit (Mitsuba)
  scripts/          the build and run scripts (copied from Windows, see below)
  sync/             manifests and patches received from Windows
```

Two rules shaped the setup:

* **Nothing is installed outside of the workspace.** Homebrew (`/opt/homebrew`) is only
  used as it is (cmake, ninja, git, git-lfs, python). The scripts never run `brew install`,
  `pip --user` or `git lfs install`.
* **Nothing is written to the home directory.** `scripts/env.sh` redirects the caches of
  pip, Python and the XDG directories into `cache/`, every CMake configuration starts from
  `scripts/common.cmake`, which switches the user package registry (`~/.cmake/packages`)
  off, and Blender runs through `scripts/blender.sh` with a home directory of its own
  ([section 5](#5-running-blender-and-the-smoke-tests)). One thing cannot be redirected:
  when a process crashes, macOS writes a crash report to `~/Library/Logs/DiagnosticReports`.

The disk is exFAT with an allocation unit of 1 MB: every file occupies at least 1 MB.
The workspace with Blender, its libraries, the eight devices and pynari takes 116 GB
(`blenderphi` 54 GB, of which the libraries 16 GB; `build/blender` 19 GB; the Python
environment of pynari 3 GB); MoonRay adds 8 GB for the disk image it is built in and 1 GB
for its installed runtime. exFAT also has no
permissions (every file is executable), no hard links, and stores extended attributes in
`._<name>` files.

## 3. Synchronizing the sources

The Windows working trees are the source of truth, and they are usually ahead of the
remotes: uncommitted changes, commits and merges which are not pushed, merges in progress.
The Mac never gets files copied from Windows (the Windows check-outs have CRLF line
endings). Instead:

1. For each repository and each of its checked out submodules, a **base commit** is
   chosen which is on the remote `origin`: HEAD if it is pushed, else the merge base of
   HEAD with its upstream branch, else the newest ancestor of HEAD which a remote branch
   has. The Mac fetches that commit from the remote.
2. A **binary patch** from the base commit to the working tree carries everything else:
   commits which are not pushed, uncommitted and untracked files (ignored files are left
   out). Git normalizes the line endings while it makes the patch.

The script is [`tools/macos/sync_to_mac.sh`](tools/macos/sync_to_mac.sh) of this
documentation (Git Bash; `MAC_HOST` is the ssh name of the Mac, by default the alias `mac`):

```bat
"C:\Program Files\Git\bin\bash.exe" F:\work\anari\blenderphi\doc\anari\tools\macos\sync_to_mac.sh blenderphi cyclesphi-anari
"C:\Program Files\Git\bin\bash.exe" F:\work\anari\blenderphi\doc\anari\tools\macos\sync_to_mac.sh all
```

`all` is ANARI-SDK, blenderphi, cyclesphi-anari, barney, visionaray, anari-visionaray,
anari-ospray, RadeonProRenderANARI, photon, mitsuba-anari and pynari. `--local` only makes
the patches (in `build\sync\macos` of the Windows workspace), `--no-apply` copies them without applying, `--scripts` only
copies `tools/macos/scripts` to `~/scratch/anari/scripts`.

On Windows the script only reads:

* The patch is made with a temporary index (a copy of the real one, so only changed files
  are read) and a temporary object directory. The working tree, the index, `MERGE_HEAD`
  and the object database of the repository are not written to.
* Nothing is fetched. The base commit is found with the remote-tracking branches as they
  are.
* A repository in the middle of a merge is sent with the content of its working tree. If a
  file still contains conflict markers, the repository is not sent and the script says
  which files (`SYNC_ALLOW_MARKERS=1` sends it anyway).

On the Mac, `scripts/sync_apply.sh` (run by the script over ssh):

* creates the repository on first use and fetches the base commit with `--depth 1` from
  the remote (`SYNC_DEPTH=0` fetches the full history). The remotes are read over https.
  Submodules are created with their git directory inside the one of the superproject.
  The Blender library submodules of other platforms are skipped;
* moves HEAD to the base commit, applies the patch **to the index** and updates the
  working tree from the index. Only files whose content changed are written, so a new sync
  only rebuilds what changed. Files which are no longer part of the state are removed;
* compares the tree id of its index with the tree id of the Windows state. A sync which
  ends with `OK` is exact;
* makes no commits. On the Mac the Windows changes show as staged changes
  (`git diff --cached`), and `git diff` shows what was edited on the Mac since the last
  sync. Such edits are saved to `sync/mac-local/` before the next sync overwrites them.

Git LFS files are fetched after the check-out. blenderphi has 6919 of them (886 MB, mostly
`tests/files`); most are not in the fork on GitHub and come from
`projects.blender.org/blender/blender.git`, as `make update` does it. The first sync of
blenderphi therefore takes about 20 minutes, later ones seconds.

[`tools/macos/mac.sh`](tools/macos/mac.sh)` '<command>'` runs a command on the Mac inside the workspace, with the
environment of `scripts/env.sh`. Both scripts use the Windows OpenSSH client
(`C:\Windows\System32\OpenSSH\ssh.exe`): the ssh of Git Bash has no access to the Windows
ssh-agent.

## 4. Building

The build scripts are in `~/scratch/anari/scripts`; the copy which is edited is
[`tools/macos/scripts`](tools/macos/scripts) of this documentation (every sync copies it). Each script can be
run again; it configures, builds and installs, and writes its logs to `build/logs`. All
builds are Release builds with Ninja.

| Script | Builds |
|---|---|
| `env.sh` | sourced by all scripts: paths, cache redirection, `JOBS` (default 8) |
| `common.cmake` | initial CMake cache of every component: no package registry, Release, arm64, `CMAKE_POLICY_VERSION_MINIMUM=3.5` |
| `build_anari_sdk.sh` | ANARI SDK with helide → `install/` |
| `fetch_blender_libs.sh` | `blenderphi/lib/macos_arm64` at the commit blenderphi records |
| `build_blender.sh [configure] [build] [install]` | Blender → `build/blender/bin/Blender.app` |
| `build_cyclesphi_anari.sh` | cyclesphi-anari → `install/lib` |
| `build_barney.sh` | barney, CPU back end → `install/lib` |
| `build_visionaray.sh` | Visionaray headers and anari-visionaray → `install` |
| `build_anari_ospray.sh` | anari-ospray and the OSPRay runtime → `install/ospray/lib` |
| `build_rpr.sh` | RadeonProRenderANARI and the RPR runtime → `install/rpr/lib` |
| `build_llvm_c.sh` | an LLVM library with only the C API exported → `install/llvm-c/lib` |
| `build_mitsuba_anari.sh` | mitsuba-anari with Mitsuba and Dr.Jit → `install/mitsuba/lib` |
| `build_photon.sh` | Kokkos → `install/kokkos-host`, Photon → `install/photon_cpu/lib` |
| `build_moonray.sh [deps] [moonray] [device] [stage]` | MoonRay, moonshine and openmoonray-anari on the APFS image → `install/moonray` |
| `test_moonray.sh [prefix]`, `moonray_test.rdla` | a render with the standalone `moonray`, [4.10](#410-moonray-openmoonray-anari) |
| `build_pynari.sh` | the Python environment `build/pynari-venv` and pynari → `build/pynari` |
| `run_pynari.sh`, `run_pynari.py` | the pynari sample matrix, [section 9](#9-pynari) |
| `metal_try.sh`, `exr_compare.py` | for work on the Metal kernels of cyclesphi, [4.3](#43-cyclesphi-anari) |
| `blender.sh`, `run_smoke.sh`, `smoke_tables.py` | [section 5](#5-running-blender-and-the-smoke-tests) |
| `bg.sh <name> <command...>` | runs a long command detached from the ssh session; `bg.sh --wait <name>` waits for it |

A non-interactive ssh shell is zsh with the system `PATH` only. Run the scripts with
`bash`; they set their own `PATH`.

### 4.1 ANARI SDK (with helide)

```sh
bash scripts/build_anari_sdk.sh
```

The same options as on Windows. The libraries go to `install/lib` (on Windows the DLLs are
in `install/bin`); all devices are built with
`-Danari_DIR=$ANARI_INSTALL/lib/cmake/anari-0.17.0`. helide builds its own static Embree.

### 4.2 Blender

```sh
bash scripts/fetch_blender_libs.sh      # once, 1.1 GB of Git LFS files, 14255 files
bash scripts/build_blender.sh           # configure, build (about 15 minutes), install
```

`fetch_blender_libs.sh` checks out the library submodule at the commit recorded in
blenderphi, with a shallow fetch. It replaces `make update`, which would also pull the
Blender sources.

Blender is configured like on Windows (`-DWITH_CYCLES_DEVICE_ANARI=ON -Danari_DIR=...
-DWITH_GTESTS=OFF`), with its default options otherwise; Cycles has its Metal device. For
16 GB of memory the script builds with 6 compile jobs, 2 heavy compile jobs and 1 link job
(`BLENDER_JOBS`, `NINJA_MAX_NUM_PARALLEL_*`).

On macOS, a Blender which was only built does not run: `cmake --install` assembles
`Blender.app`, with the bundled libraries in `Blender.app/Contents/Resources/lib`. The ANARI
front-end library is installed there as `libanari.0.dylib`
([section 9](#10-source-changes-made-for-macos)). After C++ changes run
`build_blender.sh build install`, after changes to the add-on's Python files
`build_blender.sh install`.

### 4.3 cyclesphi-anari

```sh
bash scripts/build_cyclesphi_anari.sh
```

Built against Blender's precompiled libraries (`-DCYCLES_LIB_DIR=.../lib/macos_arm64`) with
Open Image Denoise, without OptiX. The library `libanari_library_cycles.dylib` and its data
(`cycles/anari`) are installed to `install/lib`.

**Compute devices.** The CPU and the Metal device of Cycles are built
(`ANARI_CYCLES_USE_METAL`, default `ON` on Apple). `computeDevice=cpu|metal` selects one;
without the parameter the device takes the GPU if there is one, as on Windows (OptiX, CUDA,
Metal, CPU in this order).

**Metal kernels.** Cycles compiles its Metal kernels at runtime from the kernel sources.
The device installs them to `install/lib/cycles/source/{kernel,util}` (the layout of
Blender's `scripts/addons_core/cycles/source`) and finds them next to the library.

* The first use compiles the kernels: 145 s for the first render on the M4. Kernels which
  are specialized for a scene are compiled when a scene needs them (the pynari `sample07`
  took 137 s the first time, 1.6 s afterwards).
* The compiled kernels are cached in `$XDG_CACHE_HOME/cycles/kernels`, or in
  `~/.cache/cycles/kernels` when `XDG_CACHE_HOME` is not set. `scripts/env.sh` sets it to
  `cache/xdg/cache`, so with `scripts/blender.sh` and `scripts/run_pynari.sh` the cache is
  `~/scratch/anari/cache/xdg/cache/cycles/kernels` (765 MB on this disk after the tests).
  A Blender which is started in another way and uses the device writes to
  `~/.cache/cycles`, like Blender's own Cycles on Metal.
* When the device is created with Metal, it loads the kernels first, with a Cycles device
  of its own. If they do not compile, it reports the compiler errors as an ANARI error and
  renders on the CPU. Without this the error would only show up at the first render, when
  the scene already lives on the Metal device.
* A render which Cycles gives up with an error ends the frame with an ANARI error. Before,
  the frame was never finished and the application waited forever.

`scripts/metal_try.sh [test ...]` renders smoke tests with `computeDevice=metal` and prints
the kernel compile errors. It reads the kernel sources from the source tree
(`CYCLES_KERNEL_PATH`), so a kernel header can be edited and tried without building.

**What cyclesphi needed for Metal.** Blender's own Cycles compiles as Metal Shading
Language; the code cyclesphi added to the kernel did not:

* `kernel/device/metal/context_begin.h` included `<nanovdb/NanoVDB.h>`, which is not valid
  Metal (no address space qualifiers, doubles). It includes `kernel/util/nanovdb.h` again,
  the extract of NanoVDB which Cycles uses on Metal.
* `kernel/util/nanovdb.h` got the index/world transform of a grid in single precision
  (`worldToIndexF`, `indexToWorldF`, `voxelSizeF`). NanoVDB stores the transform as floats
  and as doubles; Metal has no doubles, so the float copy is read, and the voxel size, which
  only exists as doubles, is converted from its bits.
* `kernel/util/image_3d.h` (multi-resolution grids, derivative bundles, RAW3D and CUB
  grids) is still one implementation for all devices. The pointers and references got
  address space qualifiers (`ccl_global`, `ccl_private`), and the arithmetic uses
  `image_real_t`: `double` everywhere except on Metal, where it is `float`. The grids are
  read through `IMAGE_NANOVDB`, which is the NanoVDB library everywhere except on Metal,
  where it is `kernel/util/nanovdb.h`. `if constexpr` (C++17, not in Metal) was replaced by
  overloads.

What was verified on Metal and what was not:

| Volume format | On Metal |
|---|---|
| RAW3D grids (`structuredRegular` fields, what Blender sends) | rendered, relative RMS difference to the CPU 0.0001 |
| NanoVDB grids (`CYCLES_ANARI_VOLUME_RAW3D=0`) | rendered, relative RMS difference to the CPU 0.0012 |
| multi-resolution grids, derivative bundles (both formats), CUB grids | compile; **not rendered**, there is no data in these formats on the Mac |
| ZFP grids | not ported. `WITH_ZFP_LOADER` is off in this build, also for the CPU; its decoder is written for CUDA (`kernel/util/zfp/*.cuh`) |

On Metal the multi-resolution and derivative formats compute in single precision; the
other devices keep double precision and the code they compile is the same as before.

The library depends on Blender's shared libraries by `@rpath` (OpenImageIO, OpenVDB,
OpenColorIO, OpenEXR, TBB, Open Image Denoise). Inside Blender these resolve to the copies
Blender has loaded. A program other than Blender has to provide them, for instance with
`DYLD_LIBRARY_PATH=.../Blender.app/Contents/Resources/lib`.

### 4.4 barney

```sh
bash scripts/build_barney.sh
```

Only the CPU back end exists on macOS: `-DBARNEY_BACKEND_CPU=ON -DBARNEY_BACKEND_OPTIX=OFF
-DBARNEY_BACKEND_CUDA=OFF -DBARNEY_MPI=OFF`. It builds the Embree of
`barney/submodules/embree` into the library; `libanari_library_barney.dylib` (and
`libanari_library_barney_cpu.dylib`) have no dependencies besides the system libraries.

### 4.5 Visionaray

```sh
bash scripts/build_visionaray.sh
```

The script installs the Visionaray headers (as on Windows, without viewer, examples and
the `common` library; no CUDA, no TBB) and then builds anari-visionaray with the CPU device
only. anari-visionaray compiles against the installed headers in
`install/include/visionaray`, which is why the script always does both.

### 4.6 anari-ospray

```sh
bash scripts/build_anari_ospray.sh
```

The script downloads `ospray-3.2.0.arm64.macosx.zip` from the OSPRay releases to
`external/ospray` if it is not there, builds the device against it and installs it to
`install/ospray/lib`. It then copies the OSPRay libraries of the release next to the device
(without the test, example and GUI libraries). The device finds them through its rpath
(`@loader_path`), and OSPRay finds its modules next to `libospray`.

As on Windows, the device has a directory of its own because Blender ships libraries of the
same kind (Embree, TBB, Open Image Denoise). On macOS the two sets have different file
names (`libembree4.4.dylib` and Blender's `libembree4.dylib`), so both are loaded into the
Blender process side by side. The smoke tests pass that way, including OSPRay's denoiser.

### 4.7 RadeonProRenderANARI

```sh
bash scripts/build_rpr.sh
```

The script clones the RadeonProRender SDK to `external/rpr-v3.1.6` (without its `hipbin`
submodule: those are the HIP and CUDA kernels, Northstar uses Metal on macOS) and installs
the device with `libRadeonProRender64.dylib` and `libNorthstar64.dylib` to
`install/rpr/lib`. The SDK version is 3.1.6 and not 3.1.7.1 as on Windows, and only the
CPU renders correctly; both are explained in [section 7](#7-what-does-not-work).

### 4.8 mitsuba-anari

```sh
bash scripts/build_mitsuba_anari.sh      # runs build_llvm_c.sh first if needed
```

The device builds Mitsuba 3 (the submodule `ext/mitsuba3`, v3.9.1) and Dr.Jit in its
configure step, as on Windows; that took about 10 minutes with 6 jobs. On Apple the device
replaces the variant `cuda_ad_rgb` by `metal_ad_rgb` by itself, so the variants are
`scalar_rgb`, `llvm_ad_rgb` and `metal_ad_rgb`. Everything is installed to
`install/mitsuba/lib`: the device, `libmitsuba`, `libdrjit-core`, `libdrjit-extra`, the
plugins and data, and a copy of the ANARI front-end library. No source change was needed.

The variant is selected with the device parameter `mitsuba.variant` (or
`ANARI_MITSUBA_VARIANT`). When a variant is not available the device says so in a warning
and renders with `scalar_rgb`; the test logs were checked for that warning.

**Metal (`metal_ad_rgb`).** Dr.Jit generates Metal Shading Language at runtime; nothing is
compiled at build time. The first render in a fresh process compiles the kernels: the
basic smoke test took 5.9 s the first time and 0.4 s afterwards. The compiled Metal
shaders are cached by macOS in the per-user cache directory of the system
(`getconf DARWIN_USER_CACHE_DIR`, below `/var/folders`), per application; that is neither
the home directory nor the workspace, and Dr.Jit has no setting for it.

**LLVM (`llvm_ad_rgb`).** Dr.Jit loads `libLLVM.dylib` at runtime and uses its C API. With
the LLVM of Homebrew this crashes inside Blender, at the first kernel it compiles: Blender
has an LLVM of its own (in `liboslexec.dylib`), both export their C++ symbols, and dyld
binds the weak ones of the two libraries to each other
([section 6](#6-differences-from-windows)). Blender's LLVM cannot be used directly either,
`liboslexec.dylib` does not export all of the C API ("detected LLVM version lacks core API
used by Dr.Jit", and the device falls back to `scalar_rgb`). `scripts/build_llvm_c.sh`
therefore links a `libLLVM.dylib` from the static LLVM 22.1.7 libraries of Blender's
precompiled libraries which exports the C API and nothing else, the counterpart of the
`LLVM-C.dll` used on Windows. The build installs it next to the device
(`-DMITSUBA_ANARI_LLVM_RUNTIME`), where Dr.Jit finds it by name before it looks for the
one of Homebrew. `DRJIT_LIBLLVM_PATH` selects another library. The first render took 1.6 s,
later ones 0.8 s.

**Kernel cache of Dr.Jit.** `DRJIT_CACHE_DIR`, by default `~/.drjit`. `scripts/env.sh` sets
it to `cache/drjit`; `scripts/blender.sh` and `scripts/run_pynari.sh` use it, and nothing
was written to `~/.drjit`.

### 4.9 Photon

```sh
bash scripts/build_photon.sh
```

The CPU build of [WINDOWS.md](WINDOWS.md#410-photon-kokkos): Kokkos 5.2.2 with the Threads and Serial back ends
(`install/kokkos-host`, built by the same script), and Embree as the ray tracing back end.
The GPU build needs CUDA and OptiX. Blender's macOS libraries have Embree but no CMake
package for it, so the script writes a minimal one (`build/embree-blender-cmake`) which
points to `blenderphi/lib/macos_arm64/embree`. The device is installed to
`install/photon_cpu/lib`. It links `@rpath/libembree4.dylib`: inside Blender that is the
copy Blender has loaded, other programs need `Blender.app/Contents/Resources/lib` on
`DYLD_LIBRARY_PATH` (the pynari runner sets it). Tests, apps and tools of Photon are not
built. No source change was needed.

### 4.10 MoonRay (openmoonray-anari)

```sh
bash scripts/build_moonray.sh                        # deps, moonray, device, stage
bash scripts/build_moonray.sh moonray device stage   # after a sync
bash scripts/test_moonray.sh [prefix]                # a render with the standalone moonray
```

MoonRay is built from the forks which `openmoonray-anari` pins as submodules
(`external/openmoonray` with `moonray`, `moonshine`, `scene_rdl2`, `mcrt_denoise`,
`cmake_modules`), with the macOS scripts of that repository
(`scripts/build-moonray-deps-macos.sh`, `scripts/build-moonray-macos.sh`). It renders on
the CPU; XPU through Metal is not built ([section 7](#7-what-does-not-work)).

**Where it is built.** Not on the exFAT disk: clang's `#pragma once` fails there for
headers which are reached through differently spelled paths (Embree's in-source build,
scene_rdl2, moonray), and the autotools projects of the dependency chain depend on file
times. `build_moonray.sh` builds on an APFS volume instead, a sparse disk image which is
stored inside the workspace:

| Path | Content |
|---|---|
| `~/scratch/anari/apfs.sparsebundle` | the image: 200 GB at most, 8 GB in use |
| `/Volumes/anari-apfs` | where it is mounted; `~/scratch/anari/mnt` is a link to it |
| `/Volumes/anari-apfs/moonray/openmoonray-anari` | a copy of the source mirror (`rsync`, on every run), with the build directories (`build/`, 6.3 GB) and the install prefixes of the repository's scripts (`external/installs`, 0.9 GB; MoonRay in `external/installs/openmoonray`) |
| `/Volumes/anari-apfs/tmp` | `TMPDIR` of the build |

`hdiutil` refuses a mount point on the exFAT disk ("Permission denied"), so the volume is
mounted at the default place; `/Volumes/anari-apfs` is the only path outside of the
workspace, and it only exists while the image is attached. After a restart the image has
to be attached again: `build_moonray.sh` and `test_moonray.sh` do that
(`anari_apfs_mount` in `env.sh`: `hdiutil attach -nobrowse`). The installed runtime in
`install/moonray` is on the exFAT disk and does not need the image.

**Steps.**

* `deps`: the dependency chain of upstream MoonRay (`external/openmoonray/building/macOS`)
  with `NO_USD=1`, up to OpenImageDenoise: Boost 1.78, TBB 2020 U3, Embree 4.2.0,
  OpenEXR 2.5.7, OpenVDB 9.1.0, OpenImageIO 2.3.20, OpenSubdiv 3.5.0, OpenColorIO 2.0.2,
  Blosc, Lua 5.4.4, log4cplus, JsonCpp and others from source, ISPC 1.20.0 and
  OpenImageDenoise 2.2.0 as binaries. About 25 minutes. The script of the repository
  resumes the chain after each of its known failures by itself.
* `moonray`: scene_rdl2, mcrt_denoise, moonray, moonshine and the DSO of the device
  (`dso/AnariBumpNormalMap`) as standalone projects, with Unix Makefiles (the generator
  which has ISPC as a language on macOS) and `MOONRAY_USE_METAL=NO`. Under 9 minutes.
* `device`: `anari_library_moonray` from the top-level project of the repository, against
  the ANARI SDK of the workspace (`install/lib/cmake/anari-0.17.0`).
* `stage`: the device and the runtime to `install/moonray`, the layout of the Windows
  workspace: `lib/` (the device, the MoonRay libraries and the libraries of the chain),
  `bin/` (`moonray`, `rdl2_print`), `rdl2dso/` (175 DSOs) and `coredata/`. The device
  finds `rdl2dso/` relative to its own location. The step also removes the absolute run
  paths to the build tree from every binary, turns absolute install names into `@rpath`
  names, renames TBB (below) and signs the changed files again (ad hoc).

**What comes from outside of the workspace.** Nothing was installed for MoonRay. The
build uses what the machine has: Homebrew's OpenSSL 3 (`/opt/homebrew/opt/openssl@3`,
which the MoonRay libraries reference by its absolute path, also at run time),
`pkg-config`, and Xcode's Python 3.9 (`/usr/bin/python3`) for Boost.Python and the Python
module of scene_rdl2. The build runs with `HOME` set to `cache/home`. The first `deps`
run leaves one crash report in `~/Library/Logs/DiagnosticReports`: the script of the
repository tests the `luac` of Lua 5.4.4, which crashes when it is given two files, and
then patches Lua.

**Libraries which Blender also has.** The device runs inside Blender next to Blender's
own copies of most libraries MoonRay needs. MoonRay is built against the versions
upstream pins, which are older than Blender's (TBB 2020 instead of oneTBB, OpenEXR 2.5
instead of 3, ...), so it cannot use Blender's copies. Its own copies are kept apart by
their install names: dyld gives a library which asks for an install name that is already
loaded the loaded copy, whatever its version. All libraries are linked with the two-level
namespace (the default).

| Library | MoonRay's copy | Blender's install name | Choice |
|---|---|---|---|
| TBB | 2020 U3, `@rpath/libtbb.dylib`, `libtbbmalloc.dylib`, `libtbbmalloc_proxy.dylib` | the same three names (oneTBB) | own copy, **renamed** at staging: `libtbb_moonray.dylib`, `libtbbmalloc_moonray.dylib`, `libtbbmalloc_proxy_moonray.dylib`, and every reference to them rewritten |
| Embree | 4.2.0, `@rpath/libembree4.4.dylib` | `@rpath/libembree4.dylib` | own copy, other install name |
| OpenVDB | 9.1.0, `@rpath/libopenvdb.9.1.dylib` | `@rpath/libopenvdb.dylib` | own copy, other install name |
| OpenEXR, Imath | 2.5.7, static libraries inside the libraries which use them | `@rpath/libOpenEXR.dylib`, `libImath.dylib`, ... | own copy, static |
| OpenImageIO | 2.3.20, `@rpath/libOpenImageIO.2.3.dylib`, `libOpenImageIO_Util.2.3.dylib` | `@rpath/libOpenImageIO.dylib`, `libOpenImageIO_Util.dylib` | own copy, other install name |
| OpenSubdiv | 3.5.0, `@rpath/libosdCPU.3.5.0.dylib`, `libosdGPU.3.5.0.dylib` | `@rpath/libosdCPU.dylib`, `libosdGPU.dylib` | own copy, other install name |
| OpenColorIO | 2.0.2, `@rpath/libOpenColorIO.2.0.dylib` | `@rpath/libOpenColorIO.dylib` | own copy, other install name |
| OpenImageDenoise | 2.2.0 (binary release), `@rpath/libOpenImageDenoise.2.dylib`, with its own oneTBB `@rpath/libtbb.12.dylib` | `@rpath/libOpenImageDenoise.dylib` | own copy, other install name |
| LLVM | not used at run time (ISPC is a build tool) | inside `liboslexec.dylib` | nothing to do |

Without the new name for TBB the device does not load inside Blender: `dlopen` fails with
"Symbol not found: `tbb::spin_rw_mutex_v3::internal_upgrade()` ... Expected in:
`Blender.app/Contents/Resources/lib/libtbb.dylib`". After it, no install name of the 97
libraries in `install/moonray/lib` is one of Blender's, and of the weak definitions which
they export (the symbols dyld shares between images,
[section 6](#6-differences-from-windows)) one is also exported by Blender or one of its
libraries, apart from those of the C++ standard library: it is in the oneTBB of
OpenImageDenoise. The versions differ in their inline namespaces (`openvdb::v9_1`,
`Imf_2_5`, `OpenImageIO_v2_3`, ...).

**Standalone test.** `test_moonray.sh` renders `scripts/moonray_test.rdla` (a moonshine
`DwaBaseMaterial` sphere under an `EnvLight`, 320×240) with `bin/moonray` of the build
tree, or of the prefix given (`~/scratch/anari/install/moonray` for the installed
runtime), and writes `build/test_out/moonray_standalone/moonray_test.exr`.

Source changes: one line of the device and the build script of the repository,
[section 10](#10-source-changes-made-for-macos).

## 5. Running Blender and the smoke tests

```sh
bash scripts/blender.sh [blender arguments]
```

`blender.sh` starts `build/blender/bin/Blender.app/Contents/MacOS/Blender` with a home
directory inside the workspace, so that neither Blender nor an ANARI device writes to the
real one:

| Variable | Value | Covers |
|---|---|---|
| `BLENDER_USER_RESOURCES` | `cache/blender-user` | Blender's configuration, scripts, extensions |
| `HOME` | `cache/home` | POSIX code (`~/.cache`, `~/.drjit` of Mitsuba, ...) |
| `CFFIXED_USER_HOME` | `cache/home` | the macOS frameworks (`~/Library/Caches`, preferences) |
| `XDG_CACHE_HOME` | `cache/xdg/cache` | the kernel cache of Cycles |

The library search paths of the ANARI add-on are separated by `;` on macOS as well, and
they hold `libanari_library_<name>.dylib`: `install/lib`, and the directory of each device
which is installed on its own (`install/ospray/lib`, `install/rpr/lib`,
`install/mitsuba/lib`, `install/photon_cpu/lib`, `install/moonray/lib`).

The smoke tests of [README section 4](README.md#4-smoke-tests):

```sh
bash scripts/run_smoke.sh                 # every configuration whose library is installed
bash scripts/run_smoke.sh cycles cycles_metal helide
bash scripts/run_smoke.sh helide -- AnariSmokeTest.test_lights
python3 scripts/smoke_tables.py           # the tables of section 8 from the logs
```

Logs are `build/logs/smoke-<configuration>.txt`, images `build/test_out/<configuration>`.
A run which hangs is stopped after `SMOKE_TIMEOUT` seconds (3600).

## 6. Differences from Windows

* **Libraries are in `lib`.** Windows installs DLLs to `install/bin`; on macOS the device
  libraries are `install/lib/libanari_library_<name>.dylib`.
* **Blender needs the install step**, see [4.2](#42-blender).
* **Weak symbols are shared between libraries.** On Windows every DLL has its own copy of
  the inline functions and template instantiations it uses. On macOS, dyld binds the
  exported weak symbols of all loaded images to one definition. A device which contains
  code that the host application also contains, built with other options, gets mixed with
  it. cyclesphi-anari in Blender crashed this way on the first allocation: both contain
  Cycles, Blender's with Blender's guarded allocator. The device now only exports its two
  entry points ([section 10](#10-source-changes-made-for-macos)). The same thing happened
  with Mitsuba's LLVM variant: the `libLLVM.dylib` of Homebrew and the LLVM inside Blender's
  `liboslexec.dylib` ([4.8](#48-mitsuba-anari)). Any other device which brings a library
  that Blender also has, and exports its symbols, can run into it.
* **Device libraries find the front end through Blender.** Devices which link
  `libanari.0.dylib` find the copy inside `Blender.app` through Blender's rpath. helide and
  the other devices built on helium link the back-end code statically and do not need it.
* **Deployment target.** Blender is built for macOS 13.0, Cycles by default for 11.2, and
  CMake projects without a setting for the host version. `env.sh` sets 13.3 for the SDK and the
  devices: the floating point `std::to_chars` used by cyclesphi-anari does not exist
  before it. The link step of Blender then warns that `libanari.0.dylib` was built for a
  newer macOS than Blender; that is harmless on this machine.
* **Apple clang is stricter than MSVC** in the Cycles kernel: implicit conversions from
  `float` to `double` are errors there (`-Werror=double-promotion`).
* **A library with the install name of one of Blender's gets Blender's copy.** On Windows
  a DLL next to the device is found first. On macOS, dyld matches a dependency by its
  install name with the images which are already loaded, so a device which brings
  another version of `@rpath/libtbb.dylib` gets the one of Blender. MoonRay's TBB is
  renamed for that reason ([4.10](#410-moonray-openmoonray-anari)).
* **Embree and MoonRay on exFAT.** Embree's in-source build can fail on exFAT with
  "redefinition" errors (`#pragma once` does not recognize a header reached through
  differently spelled paths). The Embree copies of helide, barney and Mitsuba compiled on
  exFAT without that problem; the dependency chain of MoonRay and MoonRay itself are built
  on an APFS sparse image which is stored inside `~/scratch/anari`
  ([4.10](#410-moonray-openmoonray-anari)), so that everything stays on the external disk.
* **Line endings and file modes.** The Mac mirrors are LF check-outs. exFAT has no
  executable bit; scripts are run with `bash <script>`.

## 7. What does not work

* **Radeon ProRender on the GPU.** Northstar creates its Metal context and renders, but
  the images are wrong: surfaces lit by point, spot, area and sun lights come out black
  (the environment still lights the scene), textures are missing, and 18 checks of the
  smoke suite fail. It is the same with flattened materials (`materials=physicallyBased`).
  From pynari no frame finishes at all: the first two samples ran into the timeout of
  600 s. `computeDevice=cpu` (`ANARI_RPR_COMPUTE_DEVICE=cpu`) works. The device selects
  the GPU by default, so on macOS the CPU has to be asked for. The cause was not
  investigated.
* **Radeon ProRender, version.** The macOS libraries in the RPR SDK repository were last
  updated for 3.1.6; the tags v3.1.7 and v3.1.7.1 only updated Windows and Linux. With the
  3.1.7 headers the libraries refuse to create a context
  (`RPR_ERROR_INVALID_API_VERSION`), so the device is built against the tag v3.1.6 on
  macOS. It compiles against those headers unchanged.
* **Radeon ProRender on the CPU, pynari.** Two samples with many spheres (`sample02` and
  `interactive-bobblespheres3`) end with a bus error inside the RPR library; in one run
  `viewer-sample02` (the same scene) did as well.
* **cyclesphi-anari on Metal, once.** In the first pynari run on Metal, while kernels were
  still being compiled, one sample ended with an uncaught Objective-C exception in
  `ccl::MetalKernelPipeline::compile` (`-[NSURL initFileURLWithPath:]: nil string
  parameter`). It did not happen again in a second run of the matrix and in four runs of
  the smoke suite. That code is Cycles' own, not what was ported.
* **Mitsuba `llvm_ad_rgb`, volumes in Blender.** The `gn_volume` scene of the smoke suite
  renders a completely black image (mean 0.000; coverage 0.102, brightness ratio 0.000),
  without any warning or error. `metal_ad_rgb` and `scalar_rgb` render the same scene
  correctly (0.999 / 1.018 / 0.053 and 0.999 / 1.017 / 0.052), and the volume samples of
  pynari (`sample03`, the `unstructured` samples) come out the same with LLVM and with
  Metal. All other tests of the suite pass with LLVM and agree with Metal. The cause was
  not found; whether `llvm_ad_rgb` does the same on Windows is not known, the Windows
  results are from `cuda_ad_rgb`.
* **Mitsuba `scalar_rgb`.** The full smoke suite ends with a segmentation fault in
  `mitsuba::PathIntegrator::sample` during `test_world_camera_ray`, the last test. The
  variant is the slow fallback and not part of the default runs.
* **MoonRay XPU (Metal).** MoonRay is built with `MOONRAY_USE_METAL=NO`, so it renders on
  the CPU only. The Metal build compiles `MetalGPUPrograms.metal` with the `metal`
  compiler of Xcode, and on macOS 26 that is a component which has to be downloaded
  (`xcodebuild -downloadComponent MetalToolchain`): `xcrun metal` answers "cannot execute
  tool 'metal' due to missing Metal Toolchain". That is an installation into the system,
  so it was not done and XPU was not tried.
* **MoonRay, `image3D` samplers.** The device does not implement them (a warning, the
  default of the parameter is used), on Windows neither. The two `sampler-image3d` samples
  of pynari pass with an image which is not the intended one. Its mean brightness is 224
  of 255 here and was 241 in the Windows run of 2026-10-09; the device sources changed in
  between, and the Windows run was not repeated.
* **VisRTX** needs CUDA and OptiX.

## 8. Smoke test results

Run of 2026-10-11 (shortly after midnight) on the M4, 160×120 at 32 samples, Blender
`2220c152b37e` with the working tree changes as of the evening of 2026-10-10.

The numbers were compared with the Windows logs of 2026-10-10 (`build/logs/smoke-final-*`);
the tables of [WINDOWS.md](WINDOWS.md#6-smoke-test-results) are from 2026-10-09 and differ for the
basic render of ospray.

* **cycles (CPU):** every number is identical to Windows, also after the Metal port of the
  kernel.
* **cycles (Metal):** every number is the same as on the CPU, except `particle_hair`
  (1.000 / 0.004 instead of 1.001 / 0.006).
* **helide:** identical to Windows in all tests of the tables below. It fails
  `test_environment_map` (both rotations) and `test_camera_clip` as on Windows: it has no
  `hdri` light and no clip planes.
* **barney, ospray:** within 0.003 of the Windows numbers.
* **visionaray:** within 0.02 of Windows (its images are noisier).
* **rpr (CPU):** all tests pass. The Windows numbers are from the GPU with RPR 3.1.7; the
  basic render and the lights are within 0.001 of them.
* **rpr (default):** the GPU through Metal, see [section 7](#7-what-does-not-work).
* **photon (CPU):** all tests pass; within 0.01 of the Windows numbers, except `gn_volume`
  (1.000 / 1.000 / 0.004 here, 0.999 / 1.019 / 0.055 in WINDOWS.md, from 2026-10-09; Photon
  changed since).
* **mitsuba (Metal):** all tests pass, and every number which was compared (basic render,
  point and spot light, `gn_volume`) is identical to `cuda_ad_rgb` on Windows.
* **mitsuba (llvm):** identical to Metal in every test except `gn_volume`, which is black,
  and `particle_hair` (coverage 1.000 instead of 0.999).
* **mitsuba (scalar):** crashed in the last test, so the run printed no numbers.
* **moonray:** run later than the others, see [MoonRay](#moonray) below; it is not in the
  tables of the other configurations.

### Test status

| | cycles (CPU) | cycles (Metal) | helide | barney | visionaray (CPU) | ospray | rpr (default) | rpr (CPU) | photon (CPU) | mitsuba (llvm) | mitsuba (Metal) | mitsuba (scalar) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `test_background` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_basic_render` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_build_options` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_camera_clip` | pass | pass | **fail** | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_camera_shift` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_device_denoiser` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_engine_registered` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_environment_map` | pass | pass | **fail** | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_instancing` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_lights` | pass | pass | pass | pass | pass | pass | **fail** | pass | pass | pass | pass | pass |
| `test_normal_map` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_object_types` | pass | pass | pass | pass | pass | pass | **fail** | pass | pass | **fail** | pass | pass |
| `test_shader_graph` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_texture` | pass | pass | pass | pass | pass | pass | **fail** | pass | pass | pass | pass | pass |
| `test_texture_mapping` | pass | pass | pass | pass | pass | pass | **fail** | pass | pass | pass | pass | pass |
| `test_world_camera_ray` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | **crash** |
| result | 16 tests, 4.470 s, OK | 16 tests, 28.291 s, OK | 16 tests, 3.151 s, FAILED (failures=3) | 16 tests, 8.754 s, OK | 16 tests, 6.046 s, OK | 16 tests, 8.671 s, OK | 16 tests, 66.267 s, FAILED (failures=18) | 16 tests, 52.291 s, OK | 16 tests, 4.383 s, OK | 16 tests, 17.960 s, FAILED (failures=1) | 16 tests, 11.149 s, OK | did not finish (crash) |

### Brightness ratio and relative difference against Blender Cycles

Cells are *brightness ratio / relative difference*; for texture mapping the pattern agreement, for instancing and the lens shift the relative difference.

| | cycles (CPU) | cycles (Metal) | helide | barney | visionaray (CPU) | ospray | rpr (default) | rpr (CPU) | photon (CPU) | mitsuba (llvm) | mitsuba (Metal) | mitsuba (scalar) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| basic render | 0.999 / 0.021 | 0.999 / 0.021 | 0.518 / 0.819 | 1.010 / 0.413 | 1.133 / 0.347 | 1.023 / 0.453 | 0.219 / 0.926 | 1.043 / 0.084 | 1.018 / 0.076 | 1.050 / 0.191 | 1.050 / 0.191 | – |
| texture mapping | 1.000 | 1.000 | 0.944 | 1.000 | 1.000 | 1.000 | 0.497 | 1.000 | 1.000 | 1.000 | 1.000 | – |
| point light | 1.000 / 0.000 | 1.000 / 0.000 | 1.303 / 0.469 | 1.031 / 0.049 | 1.097 / 0.117 | 0.941 / 0.169 | – | 1.064 / 0.088 | 0.999 / 0.051 | 1.111 / 0.123 | 1.111 / 0.123 | – |
| spot light | 1.000 / 0.009 | 1.000 / 0.009 | 4.664 / 1.665 | 1.034 / 0.499 | 1.108 / 0.582 | 0.944 / 0.074 | – | 1.021 / 0.490 | 1.011 / 0.478 | 1.123 / 0.596 | 1.123 / 0.596 | – |
| area light | 1.000 / 0.000 | 1.000 / 0.000 | 0.389 / 0.725 | 0.966 / 0.039 | 1.021 / 0.036 | 0.875 / 0.186 | – | 0.998 / 0.036 | 0.966 / 0.039 | 1.041 / 0.046 | 1.041 / 0.046 | – |
| sun light | 1.000 / 0.001 | 1.000 / 0.001 | 0.953 / 0.065 | 0.974 / 0.044 | 0.971 / 0.034 | 1.113 / 0.113 | – | 0.971 / 0.036 | 0.974 / 0.043 | 1.043 / 0.046 | 1.043 / 0.046 | – |
| environment 0 deg | 1.000 / 0.002 | 1.000 / 0.002 | 2.241 / 1.666 | 0.993 / 0.021 | 0.988 / 0.047 | 0.997 / 0.040 | 0.958 / 0.104 | 0.996 / 0.012 | 0.993 / 0.031 | 0.999 / 0.005 | 0.999 / 0.005 | – |
| environment 90 deg | 1.000 / 0.001 | 1.000 / 0.001 | 2.332 / 1.737 | 0.994 / 0.020 | 0.989 / 0.043 | 0.984 / 0.052 | 0.997 / 0.041 | 0.997 / 0.010 | 0.985 / 0.034 | 0.998 / 0.006 | 0.998 / 0.006 | – |
| instancing | 0.001 | 0.001 | 0.041 | 0.021 | 0.009 | 0.039 | 0.086 | 0.002 | 0.022 | 0.008 | 0.008 | – |
| lens shift | 0.014 | 0.014 | 0.816 | 0.457 | 0.360 | 0.437 | 0.922 | 0.092 | 0.077 | 0.181 | 0.181 | – |
| clip start | 0.999 / 0.020 | 0.999 / 0.020 | 0.000 / 1.000 | 1.009 / 0.416 | 1.158 / 0.351 | 1.033 / 0.431 | 0.214 / 0.928 | 1.007 / 0.079 | 1.010 / 0.068 | 1.014 / 0.187 | 1.014 / 0.187 | – |
| normal map | 1.000 / 0.001 | 1.000 / 0.001 | 2.356 / 1.201 | 1.050 / 0.052 | 1.031 / 0.034 | 1.047 / 0.047 | 0.000 / 1.000 | 1.053 / 0.051 | 1.050 / 0.052 | 1.143 / 0.695 | 1.143 / 0.695 | – |
| world for camera rays | 1.000 / 0.004 | 1.000 / 0.004 | 1.001 / 0.318 | 0.978 / 0.053 | 0.952 / 0.116 | 0.989 / 0.099 | 0.661 / 0.746 | 0.987 / 0.029 | 0.975 / 0.078 | 0.993 / 0.019 | 0.993 / 0.019 | – |

### Object types

Cells are *mask coverage / brightness ratio / relative difference*.

| | cycles (CPU) | cycles (Metal) | helide | barney | visionaray (CPU) | ospray | rpr (default) | rpr (CPU) | photon (CPU) | mitsuba (llvm) | mitsuba (Metal) | mitsuba (scalar) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gn_instances | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.026 / 0.118 | 1.000 / 1.001 / 0.023 | 1.000 / 1.003 / 0.012 | 1.000 / 1.000 / 0.014 | 0.000 / 0.978 / 0.107 | 1.000 / 1.001 / 0.004 | 1.000 / 0.994 / 0.025 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | – |
| gn_realized | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.028 / 0.133 | 1.000 / 1.001 / 0.023 | 1.000 / 1.003 / 0.012 | 1.000 / 1.000 / 0.014 | 0.000 / 0.978 / 0.107 | 1.000 / 1.001 / 0.004 | 1.000 / 0.994 / 0.025 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | – |
| gn_points | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.030 / 0.102 | 1.000 / 1.003 / 0.011 | 1.000 / 1.000 / 0.008 | 1.000 / 0.993 / 0.021 | 0.000 / 0.966 / 0.129 | 1.000 / 1.001 / 0.003 | 1.000 / 0.995 / 0.015 | 1.000 / 1.001 / 0.008 | 1.000 / 1.001 / 0.008 | – |
| gn_curves | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.036 / 0.128 | 1.000 / 1.005 / 0.016 | 0.987 / 0.999 / 0.016 | 1.000 / 0.991 / 0.029 | 0.000 / 0.959 / 0.145 | 1.000 / 1.000 / 0.004 | 1.000 / 0.998 / 0.010 | 1.000 / 1.000 / 0.007 | 1.000 / 1.000 / 0.007 | – |
| gn_volume | 0.984 / 1.000 / 0.003 | 0.984 / 1.000 / 0.003 | 0.992 / 1.121 / 0.411 | 1.000 / 0.983 / 0.056 | 1.000 / 0.984 / 0.055 | 1.000 / 1.019 / 0.055 | 0.000 / 1.027 / 0.099 | 0.997 / 1.020 / 0.053 | 1.000 / 1.000 / 0.004 | 0.102 / 0.000 / 1.000 | 0.999 / 1.018 / 0.053 | – |
| particle_instances | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.012 / 0.078 | 1.000 / 1.000 / 0.004 | 1.000 / 1.000 / 0.003 | 1.000 / 0.997 / 0.015 | 0.000 / 0.992 / 0.062 | 1.000 / 1.001 / 0.003 | 1.000 / 0.997 / 0.019 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | – |
| particle_hair | 1.000 / 1.001 / 0.006 | 1.000 / 1.000 / 0.004 | 0.915 / 1.009 / 0.047 | 0.999 / 1.001 / 0.008 | 0.997 / 1.001 / 0.007 | 1.000 / 0.996 / 0.016 | 0.000 / 0.984 / 0.073 | 0.997 / 1.002 / 0.005 | 0.997 / 0.996 / 0.014 | 1.000 / 0.995 / 0.021 | 0.999 / 0.995 / 0.022 | – |
| hair_curves | 1.000 / 1.002 / 0.021 | 1.000 / 1.002 / 0.021 | 1.000 / 1.060 / 0.184 | 1.000 / 1.009 / 0.030 | 1.000 / 1.002 / 0.017 | 0.999 / 0.991 / 0.033 | 0.000 / 0.965 / 0.112 | 1.000 / 1.002 / 0.021 | 1.000 / 0.999 / 0.022 | 1.000 / 1.002 / 0.022 | 1.000 / 1.002 / 0.022 | – |
| text | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.009 / 0.043 | 1.000 / 1.001 / 0.006 | 1.000 / 1.000 / 0.005 | 1.000 / 0.995 / 0.021 | 0.000 / 0.983 / 0.078 | 1.000 / 1.002 / 0.002 | 1.000 / 0.998 / 0.010 | 1.000 / 1.000 / 0.005 | 1.000 / 1.000 / 0.005 | – |
| bevel_curve | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.022 / 0.080 | 1.000 / 1.004 / 0.016 | 1.000 / 0.999 / 0.010 | 1.000 / 0.990 / 0.038 | 0.000 / 0.958 / 0.166 | 1.000 / 1.000 / 0.004 | 1.000 / 0.997 / 0.012 | 1.000 / 1.000 / 0.007 | 1.000 / 1.000 / 0.007 | – |
| metaball | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.015 / 0.125 | 1.000 / 1.003 / 0.017 | 1.000 / 1.000 / 0.008 | 1.000 / 0.995 / 0.033 | 0.000 / 0.972 / 0.185 | 1.000 / 1.001 / 0.003 | 1.000 / 0.998 / 0.012 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | – |
| subdivision | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.013 / 0.119 | 1.000 / 1.002 / 0.018 | 1.000 / 1.001 / 0.011 | 1.000 / 0.996 / 0.036 | 0.000 / 0.976 / 0.176 | 1.000 / 1.002 / 0.003 | 1.000 / 0.997 / 0.015 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | – |

### MoonRay

`bash scripts/run_smoke.sh moonray`, run of 2026-10-11 in the morning, after a new sync of
blenderphi and openmoonray-anari (Blender `2220c152b37e` and openmoonray-anari `38eb37c`,
each with the working tree changes of that morning). The Windows column is
`build/logs/smoke-final-moonray.txt` of the same day. All 16 tests pass on both; the
largest difference between the two columns is 0.005.

| | macOS | Windows |
|---|---|---|
| result | 16 tests, 14.813 s, OK | 16 tests, 63.261 s, OK |
| basic render | 1.051 / 0.059 | 1.051 / 0.058 |
| texture mapping | 1.000 | 1.000 |
| point light | 1.085 / 0.097 | 1.085 / 0.097 |
| spot light | 1.069 / 0.074 | 1.071 / 0.079 |
| area light | 1.004 / 0.007 | 1.005 / 0.007 |
| sun light | 1.007 / 0.008 | 1.007 / 0.008 |
| environment 0 deg | 0.994 / 0.018 | 0.994 / 0.017 |
| environment 90 deg | 0.997 / 0.011 | 0.997 / 0.011 |
| instancing | 0.007 | 0.007 |
| lens shift | 0.044 | 0.043 |
| clip start | 1.051 / 0.055 | 1.051 / 0.052 |
| normal map | 1.051 / 0.049 | 1.051 / 0.049 |
| world for camera rays | 0.986 / 0.033 | 0.985 / 0.034 |
| gn_instances | 1.000 / 0.999 / 0.003 | 1.000 / 1.000 / 0.003 |
| gn_realized | 1.000 / 0.999 / 0.003 | 1.000 / 0.999 / 0.003 |
| gn_points | 1.000 / 1.000 / 0.003 | 1.000 / 1.000 / 0.003 |
| gn_curves | 1.000 / 1.022 / 0.075 | 1.000 / 1.022 / 0.076 |
| gn_volume | 1.000 / 1.001 / 0.004 | 1.000 / 1.001 / 0.004 |
| particle_instances | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.001 |
| particle_hair | 0.999 / 1.006 / 0.027 | 0.999 / 1.006 / 0.027 |
| hair_curves | 1.000 / 1.022 / 0.066 | 1.000 / 1.024 / 0.070 |
| text | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |
| bevel_curve | 1.000 / 0.999 / 0.004 | 1.000 / 0.999 / 0.004 |
| metaball | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |
| subdivision | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |

The cells are those of the tables above: brightness ratio / relative difference, and
mask coverage / brightness ratio / relative difference for the object types.

## 9. pynari

pynari is built and run as on Windows ([`pynari/doc/windows/README.md`](../../../pynari/doc/windows/README.md)),
with the same sample matrix.

```sh
bash scripts/build_pynari.sh
bash scripts/run_pynari.sh                           # every installed configuration
bash scripts/run_pynari.sh -r helide,barney -s sample0
bash scripts/run_pynari.sh -r ospray --results build/pynari-out/results-ospray.json
```

* **Python.** `build_pynari.sh` creates the environment `build/pynari-venv` from Homebrew's
  Python 3.13 (the version used on Windows) and installs numpy, pybind11, Pillow, matplotlib
  and mpi4py from PyPI into it; the pip cache is `cache/pip`. mpi4py is built against
  Homebrew's Open MPI and only used by `sample06`.
* **Module.** `build/pynari/pynari.cpython-313-darwin.so`, used from the build directory.
* **Runner.** `scripts/run_pynari.py` imports `pynari/doc/windows/tools/run_all.py` and
  replaces its paths, library names and configurations; the samples, the checks and the
  result files are those of Windows. The options are the same (`-r`, `-s`, `-t`, `-j`,
  `--results`); runs at the same time need different `--results` files. Results are in
  `build/pynari-out`: `<configuration>/<sample>.png` and `.log`, `results*.json` and `.md`.
* **Finding the libraries.** Windows registers DLL directories. On macOS the front-end
  library finds the device libraries next to itself (`install/lib`); devices in other
  directories, and the libraries a device depends on, are found through
  `DYLD_LIBRARY_PATH`, which the runner sets per configuration. For cyclesphi-anari that is
  `Blender.app/Contents/Resources/lib`. `DYLD_LIBRARY_PATH` does not survive a start through
  a system binary such as `/usr/bin/env` or `/bin/sh`, so the samples are started with the
  interpreter directly.
* **Home directory.** The samples run with `HOME` and `CFFIXED_USER_HOME` set to
  `cache/home`, and `MPLCONFIGDIR` to `cache/matplotlib`.

| Configuration | ANARI library | Selected with | Samples passed | Time of all 36 |
|---|---|---|---|---|
| `helide` | helide | | 36 of 36 | 11 s |
| `barney` | barney (CPU back end) | | 36 of 36 | 198 s |
| `cycles_cpu` | cycles | `ANARI_CYCLES_FORCE_CPU=1` | 36 of 36 | 70 s |
| `cycles_metal` | cycles | `CYCLES_ANARI_USE_GPU=METAL` | 36 of 36 | 73 s |
| `visionaray` | visionaray | | 36 of 36 | 89 s |
| `ospray` | ospray | | 36 of 36 | 83 s |
| `rpr_cpu` | rpr | `ANARI_RPR_COMPUTE_DEVICE=cpu` | 35 of 36: `interactive-bobblespheres3` ends with a bus error in the RPR library; in other runs `sample02` and `viewer-sample02` did as well | 595 s |
| `rpr` | rpr | `ANARI_RPR_COMPUTE_DEVICE=gpu` | does not finish a frame (timeout); not in the default matrix | |
| `photon_cpu` | photon | | 36 of 36 | 143 s |
| `mitsuba_llvm` | mitsuba | `ANARI_MITSUBA_VARIANT=llvm_ad_rgb` | 36 of 36 | 50 s |
| `mitsuba_metal` | mitsuba | `ANARI_MITSUBA_VARIANT=metal_ad_rgb` | 36 of 36 | 32 s |
| `moonray` | moonray | | 36 of 36 | 93 s |

Run of 2026-10-11 with two jobs in parallel (`results-final2.json`), with the kernels of
cyclesphi and Mitsuba already cached. A sample passes when its process exits normally and
wrote its image; none of the images of the passing samples is black or flat. The 36 samples
are the 25 scripts in `samples/`, the six scripts in `testing/`, the interactive sample and
the four viewer examples, the latter two driven headless by the tools of the Windows
documentation. The images of `mitsuba_llvm` and `mitsuba_metal` have the same mean
brightness in every sample.

The `moonray` row is from a run of its own, later on 2026-10-11 with one job
(`-r moonray -j 1 -t 1500`, `results-moonray.json`). Its images have the mean brightness
of the Windows run of 2026-10-09 (`results-migrated-moonray.json`, also 36 of 36) within
2 of 255 in every sample, except the two `sampler-image3d` samples
([section 7](#7-what-does-not-work)). The Windows run took 1153 s, of which `sample03`
398 s; here `sample03` takes 4 s.

The failures of `rpr_cpu` and the `rpr` configuration are specific to macOS (the RPR
libraries for macOS); everything else is as on Windows.

## 10. Source changes made for macOS

All of them are working tree changes on Windows, like the rest.

**blenderphi**

* `source/creator/CMakeLists.txt`: the Apple branch installs the ANARI front-end library
  into `Blender.app/Contents/Resources/lib`, under the file name of its install name
  (`libanari.0.dylib`). Only the Windows branch installed it. Blender links
  `@rpath/libanari.0.dylib`, and the only rpath of the installed executable is that
  directory.

The ANARI device itself (`intern/cycles/device/anari`) compiled with Apple clang without
changes.

**cyclesphi-anari**

* `device/CMakeLists.txt`:
  * The device gets the definition `WITH_SSE2NEON` when the Cycles libraries are built
    with it (ARM). Without it, the Cycles headers which the device includes do not
    compile (`util/half.h`: NEON types unknown).
  * On Apple, the device links the `Foundation` and `IOKit` frameworks, which the Cycles
    libraries use.
  * On Apple, the device only exports `anari_library_cycles_new_library` and
    `anariNewCyclesDevice` (`-exported_symbol`), see
    [section 6](#6-differences-from-windows).
  * On Apple, the device installs the kernel sources of Cycles
    (`<library directory>/cycles/source`), which the Metal device compiles at runtime.
* `CMakeLists.txt`: the option `ANARI_CYCLES_USE_METAL` (Apple only, default `ON`) for the
  Metal device of Cycles.
* `device/Device.cpp`:
  * Metal is part of the automatic device selection (after OptiX and CUDA, before the
    CPU). Cycles only lists Metal devices on macOS.
  * On Apple, the Metal kernels are loaded when the device is selected; if that fails,
    the device reports the error and renders on the CPU.
  * A session which ends with an error ends the frame in flight
    (`FrameOutputDriver::renderFailed`), on every platform. Before, a failed render left
    `anariFrameReady(ANARI_WAIT)` and `anariMapFrame` waiting forever.
* `device/frame/FrameOutputDriver.h`, `.cpp`: `renderFailed()`; `renderEnd()` returns when
  no render is in flight.
* `cyclesphi/src/kernel/device/metal/context_begin.h`, `kernel/util/nanovdb.h`,
  `kernel/util/image_3d.h`: the Metal port of the volume code, see
  [4.3](#43-cyclesphi-anari). `image_3d.h` also has explicit conversions where a `float`
  was converted to `double` implicitly, which Apple clang rejects in the kernel
  (`-Werror=double-promotion`), on the CPU as well. These files are compiled into the
  kernels of every device, so the CUDA and OptiX kernels on Windows have to be rebuilt.

**anari-ospray**, **RadeonProRenderANARI**

* `CMakeLists.txt` of each: on Apple the install rpath of the device is `@loader_path`
  instead of `$ORIGIN`, which dyld does not know. The device finds the OSPRay or RPR
  libraries next to it.

**openmoonray-anari**

* `scripts/build-moonray-macos.sh`: after moonray, the script also builds and installs
  moonshine and the DSO project of the device (`dso/`), like `-Target moonshine` and
  `-Target anari_dso` of the Windows script; moonshine is in the list of sources which the
  script copies off an exFAT volume. Before, the macOS script ended after moonray.
* `src/anari_library_moonray/backend/MoonrayScene.h`: the include of
  `scene_rdl2/render/util/AtomicFloat.h` ahead of the first use of `std::atomic<float>`
  was made for Linux only; the guard is now `defined(__linux__) || defined(__APPLE__)`.
  Apple clang rejects the file otherwise ("explicit specialization of
  'std::atomic<float>' after instantiation").

MoonRay, moonshine, scene_rdl2 and mcrt_denoise (the submodules of
`external/openmoonray`) compiled as they are.

barney, Visionaray, anari-visionaray, pynari, mitsuba-anari (with Mitsuba and Dr.Jit) and
Photon built without changes.
