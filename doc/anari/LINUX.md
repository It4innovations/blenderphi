# ANARI rendering in Blender on Linux

[WINDOWS.md](WINDOWS.md) describes the ANARI stack as it is built and tested on Windows
(`F:\work\anari`), and [MACOS.md](MACOS.md) the port to macOS. This document covers the
same stack on Linux: how the sources get to the Linux machine, how each component is built
there, what differs from Windows and what does not work.

Everything here was done on one machine, a GPU node of IT4Innovations: Rocky Linux 10.2
(kernel 6.12, glibc 2.39), an AMD EPYC 9555 (64 cores, AVX-512), 187 GB of memory, two NVIDIA
RTX PRO 6000 Blackwell (98 GB each, driver 610.43), no display. GCC 14.3.1, CUDA 13.3, CMake
4.2.1 and Ninja 1.12.1.

Contents:

1. [State](#1-state)
2. [Workspace](#2-workspace)
3. [Synchronizing the sources](#3-synchronizing-the-sources)
4. [Building](#4-building)
5. [Running Blender, the smoke tests and pynari](#5-running-blender-the-smoke-tests-and-pynari)
6. [Differences from Windows](#6-differences-from-windows)
7. [What does not work](#7-what-does-not-work)
8. [Smoke test results](#8-smoke-test-results)
9. [pynari results](#9-pynari-results)
10. [Source changes made for Linux](#10-source-changes-made-for-linux)

## 1. State

State of 2026-10-11.

| Component | ANARI library | Linux | Smoke tests in Blender | pynari samples |
|---|---|---|---|---|
| ANARI-SDK 0.17 with helide | `helide` | built | 18 run; 3 fail, see [section 7](#7-what-does-not-work) | 36 of 36 |
| Blender (`main-anari`) with the ANARI device | | built, headless | runs the tests | |
| cyclesphi-anari, CPU | `cycles` | built | 18 of 18 | 36 of 36 |
| cyclesphi-anari, OptiX | `cycles` | built | 18 of 18 | 36 of 36 |
| cyclesphi-anari, CUDA | `cycles` | built | 18 of 18 | 36 of 36 |
| barney (OptiX) | `barney` | built | 18 of 18 | 36 of 36 |
| VisRTX, RTX device | `visrtx` | built | 18 of 18, `default` and `quality` renderer | 36 of 36 |
| anari-visionaray, CPU | `visionaray` | built, with a fix | 18 of 18 | 36 of 36 |
| anari-visionaray, CUDA | `visionaray_cuda` | built | 18 of 18 | 36 of 36 |
| anari-ospray with OSPRay 3.2.0 | `ospray` | built | 17 of 18: **crashes** in the volume of `test_object_types` | 36 of 36 |
| mitsuba-anari, `cuda_ad_rgb` | `mitsuba` | built, with a fix | 17 of 18: `test_emissive_room` fails | 36 of 36 |
| mitsuba-anari, `llvm_ad_rgb` | `mitsuba` | built | 16 of 18: `test_emissive_room` fails, the volume of `test_object_types` is not rendered | 36 of 36 |
| Photon, Kokkos Cuda with OptiX | `photon` | built, with a fix | 18 of 18 | 36 of 36 |
| Photon, Kokkos Threads with Embree | `photon` | built, with a fix | 18 of 18 | 36 of 36 |
| RadeonProRenderANARI, GPU | `rpr` | built; **no GPU rendering on Blackwell**, the device falls back to the CPU | 18 of 18 (on the CPU) | 33 of 36 (on the CPU) |
| RadeonProRenderANARI, CPU | `rpr` | built | 18 of 18 | as above |
| pynari | | built | | |
| MoonRay (scene_rdl2, mcrt_denoise, moonray, moonshine) | | built, with one fix; the standalone `moonray` renders the upstream test scenes | | |
| openmoonray-anari, CPU | `moonray` | built, with one fix | 18 of 18; the mesh light tests of the device 11 of 11 | 36 of 36 |
| openmoonray-anari, XPU (OptiX) | `moonray` | built (second build) | 18 of 18; the mesh light tests of the device 11 of 11 | not run (the samples cannot select the mode) |

The details of what fails are in [section 7](#7-what-does-not-work).

## 2. Workspace

The workspace is one directory on a project file system (NFS), `<workspace>` below, with
the directory names of the Windows workspace:

```
anari/
  <repository>/     source mirrors (ANARI-SDK, blenderphi, cyclesphi-anari, barney, ...)
  blenderphi/lib/linux_x64/   Blender's precompiled libraries (Git LFS, 2.2 GB)
  build/<name>/     build directories
  build/blender/bin/blender   Blender, with its scripts and bundled libraries (bin/lib)
  build/pynari/, build/pynari-venv/   the pynari module and its Python environment
  build/logs/       build logs (<component>-configure|build|install.log), smoke-<name>.txt
  build/test_out/<name>/   images written by the smoke tests
  build/pynari-out/ results of the pynari samples
  install/
    lib/            ANARI SDK, helide, barney, VisRTX, anari-visionaray (CPU and CUDA),
                    cyclesphi-anari (+ lib/cycles), lib/cmake/anari-0.17.0
    include/visionaray, lib/cmake/visionaray
    ospray/lib/     anari-ospray and the OSPRay run time (Embree, Open VKL, OIDN, TBB)
    mitsuba/lib/    mitsuba-anari, Mitsuba and Dr.Jit, plugins/, data/
    rpr/lib/        RadeonProRenderANARI, libRadeonProRender64.so, libNorthstar64.so,
                    hipbin/ (GPU kernels), cache/ (written at run time)
    photon/lib/     Photon, GPU build (Kokkos Cuda, OptiX)
    photon_cpu/lib/ Photon, CPU build (Kokkos Threads, Embree)
    kokkos-cuda/, kokkos-host/   the two Kokkos installs
    moonray/        MoonRay and openmoonray-anari: lib/, bin/ (moonray, rdl2_*, ...),
                    rdl2dso/ (shader DSOs), coredata/, include/
    moonray_xpu/    the same, built with the GPU path (+ shaders/OptixGPUPrograms.ptx)
    moonray-deps/   Boost, JsonCpp, Log4cplus, CppUnit, Random123 for MoonRay
  external/         ospray/ (OSPRay 3.2.0 binary release), rpr/ (RadeonProRender SDK
                    3.1.7.1 with the hipbin kernels), kokkos/ (Kokkos 5.2.2 sources),
                    moonray-deps/ (sources of MoonRay's third party libraries, the
                    ISPC headers of Embree), optix-8.1/ (OptiX headers for MoonRay XPU)
  tools/            downloaded build tools: cmake, ninja, git-lfs, ispc (tools/bin)
  cache/            pip, XDG, Python, CUDA and OptiX caches, tmp/, home/, blender-user/
  scripts/          the build and run scripts (copied from Windows, see below)
  sync/             manifests and patches received from Windows
```

Two rules shaped the setup:

* **Nothing is installed outside of the workspace.** No `dnf`, no `pip --user`, no
  `git lfs install`. The build tools the node lacks are unpacked into `tools/`.
* **Nothing is written to the home directory.** `scripts/env.sh` redirects the caches of
  pip, Python, the XDG directories, CUDA (`CUDA_CACHE_PATH`), OptiX (`OPTIX_CACHE_PATH`) and
  temporary files (`TMPDIR`) into `cache/`. Every CMake configuration starts from
  `scripts/common.cmake`, which switches the user package registry (`~/.cmake/packages`)
  off. Blender and the pynari samples run with a home directory of their own (`cache/home`,
  where Dr.Jit keeps its kernel cache `.drjit`). The environment also has a runtime
  directory of its own (`XDG_RUNTIME_DIR`), no session bus address and no core dumps, see
  [section 7](#7-what-does-not-work) for why.

The workspace with Blender, its libraries, all devices and pynari takes 25 GB.

## 3. Synchronizing the sources

The method is the one of the Mac ([MACOS.md, section 3](MACOS.md#3-synchronizing-the-sources)):
the Windows working trees are the source of truth, the Linux machine never gets files copied
from Windows (they are CRLF check-outs). For each repository and each checked out submodule
the Linux side fetches a **base commit** which exists on the remote `origin`, and a **binary
patch** made on Windows with a temporary index carries everything else (commits which are
not pushed, uncommitted and untracked files). The tree id of the result is compared with the
tree id of the Windows state: a sync which ends with `OK` is exact.

The script is a generalized copy of the Mac one, with the host and the workspace as
parameters (`LINUX_HOST`, the ssh name of the machine; `LINUX_ROOT`, the workspace
directory there). Run it in Git Bash:

```bat
set LINUX_HOST=<ssh name of the machine>
set LINUX_ROOT=<workspace>
"C:\Program Files\Git\bin\bash.exe" F:\work\anari\blenderphi\doc\anari\tools\linux\sync_to_linux.sh blenderphi cyclesphi-anari
"C:\Program Files\Git\bin\bash.exe" F:\work\anari\blenderphi\doc\anari\tools\linux\sync_to_linux.sh all
```

`all` is ANARI-SDK, blenderphi, cyclesphi-anari, barney, VisRTX, visionaray,
anari-visionaray, anari-ospray, RadeonProRenderANARI, photon, mitsuba-anari and pynari;
openmoonray-anari is only sent when it is named (its nested submodules, the MoonRay forks,
are sent with it; `external/vcpkg` is skipped). `--local` only makes the patches (in
`build\sync\linux` of the Windows workspace), `--no-apply` copies them without applying, `--scripts` only copies
`tools/linux/scripts` to `scripts/` of the workspace.

On Windows the script only reads (no fetch, no change to the index or the working tree). On
Linux, `scripts/sync_apply.sh` makes no commits: the Windows changes show as staged changes
(`git diff --cached`), and `git diff` shows what was edited on Linux since the last sync.
Such edits are saved to `sync/linux-local/` before the next sync overwrites them. Source
fixes are therefore made on Windows and synchronized, not made on Linux.

The first sync of blenderphi takes about 20 minutes because of its Git LFS files (886 MB,
most of them from `projects.blender.org`), later ones seconds. The remotes are read over
https, anonymously.

[`tools/linux/linux.sh`](tools/linux/linux.sh)` '<command>'` runs a command on the Linux machine inside the workspace, with
the environment of `scripts/env.sh`. Both scripts use the Windows OpenSSH client
(`C:\Windows\System32\OpenSSH\ssh.exe`): the ssh of Git Bash has no access to the Windows
ssh-agent. `tools/linux/compare_smoke.py` and `tools/linux/compare_all.py` compare smoke test logs
fetched from the Linux machine with the Windows logs in `F:\work\anari\build\logs`.

## 4. Building

The build scripts are in `scripts/` of the workspace; the copy which is edited is
[`tools/linux/scripts`](tools/linux/scripts) of this documentation (every sync copies it). Each script can be
run again; it configures, builds and installs, and writes its logs to `build/logs`. All
builds are Release builds with Ninja and 32 jobs (`JOBS`).

| Script | Does |
|---|---|
| `env.sh` | sourced by all scripts: paths, cache redirection, the CUDA toolkit, `JOBS` |
| `common.cmake` | initial CMake cache of every component: no package registry, Release, `CMAKE_INSTALL_LIBDIR=lib`, `CMAKE_POLICY_VERSION_MINIMUM=3.5` |
| `fetch_tools.sh` | downloads CMake 4.2.1, Ninja 1.12.1 and git-lfs 3.7.1 into `tools/` |
| `build_anari_sdk.sh` | ANARI SDK with helide → `install/` |
| `fetch_blender_libs.sh` | `blenderphi/lib/linux_x64` at the commit blenderphi records |
| `build_blender.sh [configure] [build] [install]` | Blender → `build/blender/bin/blender` |
| `build_cyclesphi_anari.sh` | cyclesphi-anari with CPU, CUDA and OptiX → `install/lib` |
| `build_barney.sh` | barney, OptiX back end → `install/lib` |
| `build_visrtx.sh` | VisRTX, RTX device → `install/lib` |
| `build_visionaray.sh` | Visionaray headers and anari-visionaray (CPU and CUDA) → `install` |
| `build_anari_ospray.sh` | anari-ospray and the OSPRay run time → `install/ospray/lib` |
| `build_mitsuba_anari.sh` | mitsuba-anari with Mitsuba 3 and Dr.Jit → `install/mitsuba/lib` |
| `build_photon.sh [kokkos] [gpu] [cpu]` | Kokkos twice, Photon twice → `install/photon/lib`, `install/photon_cpu/lib` |
| `build_rpr.sh` | RadeonProRenderANARI and the RPR run time → `install/rpr/lib` |
| `build_moonray_deps.sh` | the third party libraries of MoonRay which Blender does not have → `install/moonray-deps`, ISPC → `tools/` |
| `build_moonray.sh [step ...]` | MoonRay and openmoonray-anari → `install/moonray` (`MOONRAY_XPU=ON`: `install/moonray_xpu`) |
| `build_pynari.sh` | the Python environment and the pynari module → `build/pynari` |
| `blender.sh`, `run_smoke.sh`, `smoke_tables.py` | [section 5](#5-running-blender-the-smoke-tests-and-pynari) |
| `run_pynari.sh`, `run_pynari.py`, `pynari_table.py` | [section 5](#5-running-blender-the-smoke-tests-and-pynari) |
| `bg.sh <name> <command...>` | runs a long command detached from the ssh session; `bg.sh --wait <name>` waits for it |

Run the scripts with `bash`; they set their own environment:

```sh
cd <workspace>
bash scripts/fetch_tools.sh
bash scripts/build_anari_sdk.sh
bash scripts/fetch_blender_libs.sh
bash scripts/build_blender.sh
bash scripts/build_cyclesphi_anari.sh     # needs the barney sources for the OptiX headers
bash scripts/build_pynari.sh
bash scripts/build_barney.sh
bash scripts/build_visrtx.sh
bash scripts/build_visionaray.sh
bash scripts/build_anari_ospray.sh
bash scripts/build_mitsuba_anari.sh
bash scripts/build_photon.sh
bash scripts/build_rpr.sh
bash scripts/build_moonray_deps.sh
bash scripts/build_moonray.sh
MOONRAY_XPU=ON bash scripts/build_moonray.sh    # optional: the GPU build
```

Every build script ends with `ldd -r` on the device library it installed: a shared library
with unresolved symbols links without an error and only fails when it is loaded.

### 4.1 Tools and compilers

The node has GCC 14.3.1, git, Python 3.12 with its development package, and the CUDA 13.3
toolkit of NVIDIA's RPMs in `/usr/local/cuda-13.3`. It has no CMake, Ninja or (usable)
git-lfs; `fetch_tools.sh` downloads the official binaries into `tools/`.

* **CUDA.** `/usr/local/cuda-13.3` is used (`ANARI_CUDA_ROOT`). The CUDA 12 environment
  module of the node could not be used (its compiler did not run for this account). No
  module is loaded by default; `ANARI_MODULES` loads some if needed. Every component built
  here compiles with CUDA 13.3; none needed CUDA 12.
* **git-lfs.** The git-lfs found on the `PATH` was a program for another architecture.
  `tools/bin` comes first on the `PATH`, and `env.sh` passes the LFS filter settings to git
  through the environment (`GIT_CONFIG_*`), so nothing depends on the git configuration of
  the account.
* **GPU architecture.** The GPUs are Blackwell, compute capability 12.0 (`sm_120`,
  `ANARI_CUDA_ARCH=120`), which nvcc knows since CUDA 12.8. CUDA 13 no longer builds for
  anything older than Turing (`sm_75`). What each component is built for:

  | Component | CUDA target |
  |---|---|
  | cyclesphi-anari | `sm_120` cubin, `compute_75` PTX as the fallback, OptiX kernels |
  | barney, anari-visionaray, pynari | `CMAKE_CUDA_ARCHITECTURES=120` |
  | Kokkos, Photon | `Kokkos_ARCH_BLACKWELL120`, `CMAKE_CUDA_ARCHITECTURES=120` |
  | VisRTX | `VISRTX_MIN_ARCH=75`: PTX which OptiX and the driver compile for the GPU, see [4.6](#46-visrtx) |
  | mitsuba-anari | none: Dr.Jit generates its kernels at run time |
  | RadeonProRenderANARI | precompiled kernels of the SDK, `sm_75` to `sm_90` only, see [section 7](#7-what-does-not-work) |
  | MoonRay XPU | PTX of its OptiX programs (nvcc default target), compiled by OptiX for the GPU at run time |

### 4.2 ANARI SDK (with helide)

```sh
bash scripts/build_anari_sdk.sh
```

The options of Windows, plus `-DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
-DEMBREE_IGNORE_CMAKE_CXX_FLAGS=OFF`: the GCC of Rocky Linux 10 compiles for x86-64-v3 (AVX2)
by default, and helide's own Embree, which is configured for SSE4.2 only, does not link that
way, see [section 6](#6-differences-from-windows). The libraries go to `install/lib` (on
Windows the DLLs are in `install/bin`); all devices are built with
`-Danari_DIR=$ANARI_INSTALL/lib/cmake/anari-0.17.0`.

### 4.3 Blender

```sh
bash scripts/fetch_blender_libs.sh      # once, 2.2 GB of Git LFS files, 15035 files
bash scripts/build_blender.sh           # configure, build (about 15 minutes), install
```

`fetch_blender_libs.sh` checks out the library submodule at the commit recorded in
blenderphi, with a shallow fetch. It replaces `make update`, which would also pull the
Blender sources.

Blender is configured like on Windows (`-DWITH_CYCLES_DEVICE_ANARI=ON -Danari_DIR=...
-DWITH_GTESTS=OFF -DWITH_CYCLES_CUDA_BINARIES=OFF -DWITH_CYCLES_DEVICE_HIP=OFF
-DWITH_CYCLES_DEVICE_ONEAPI=OFF`), with three more options:

| Option | Why |
|---|---|
| `-DWITH_HEADLESS=ON` | The node has no display and no X11 or Wayland development packages. Blender has no window; background rendering and Python work. |
| `-DWITH_OPENGL_BACKEND=OFF` | A headless Blender creates its OpenGL context with EGL, and the EGL headers (`EGL/eglplatform.h`; package `libglvnd-devel`) are not installed. The Vulkan back end, which only needs Blender's precompiled libraries, stays on. |
| `-DWITH_OPENAL=OFF` | OpenAL probes the audio servers of the machine during a render, also with `-noaudio`, see [section 7](#7-what-does-not-work). |

`BLENDER_HEADLESS=OFF`, `BLENDER_OPENGL=ON` and `BLENDER_OPENAL=ON` switch them back on a
machine which has the packages.

On Linux, a Blender which was only built does not run: `cmake --install` copies the scripts
and the bundled shared libraries next to the executable, into `build/blender/bin` and
`build/blender/bin/lib`. The ANARI front-end library is installed there as `libanari.so.0`
([section 10](#10-source-changes-made-for-linux)). After C++ changes run
`build_blender.sh build install`, after changes to the add-on's Python files
`build_blender.sh install`.

### 4.4 cyclesphi-anari

```sh
bash scripts/build_cyclesphi_anari.sh
```

Built against Blender's precompiled libraries (`-DCYCLES_LIB_DIR=.../lib/linux_x64`) with
OptiX and Open Image Denoise, as on Windows; the OptiX 9.0 headers are the ones of
`barney/submodules/optix`. The GPU kernels are precompiled for
`-DANARI_CYCLES_CUDA_ARCHS="sm_120;compute_75"`:

* `kernel_sm_120.cubin.zst` is the native kernel for the Blackwell GPUs. Cycles looks for
  the cubin of the GPU first and uses it here.
* `kernel_compute_75.ptx.zst` is the fallback for other GPUs (from Turing on), which the
  driver compiles when it is first used.
* `kernel_optix*.ptx.zst` are the OptiX kernels, which do not depend on the architecture.

The library `libanari_library_cycles.so` and its data (`cycles/lib`, `cycles/anari`) are
installed to `install/lib`. With 12 jobs the build takes about 7 minutes, the kernels
included.

The kernel sources built here include the changes of the Metal port
(`cyclesphi/src/kernel/util/image_3d.h`, `nanovdb.h`, `device/metal/context_begin.h`): they
compile with nvcc 13.3 for CUDA and OptiX, and the smoke test numbers are those of Windows
([section 8](#8-smoke-test-results)).

The library depends on Blender's shared libraries by name (OpenImageIO, OpenVDB,
OpenColorIO, OpenEXR, Imath, Open Image Denoise). Inside Blender these are the copies Blender
has loaded. A program other than Blender has to provide them, with
`LD_LIBRARY_PATH=.../build/blender/bin/lib`.

### 4.5 barney

```sh
bash scripts/build_barney.sh
```

The default configuration, as on Windows: the OptiX back end, with
`-DCMAKE_CUDA_ARCHITECTURES=120` and without MPI. `libanari_library_barney.so` (and
`libanari_library_barney_optix.so`) go to `install/lib`. The Embree of
`barney/submodules/embree` is not built (it belongs to the CPU back end).

### 4.6 VisRTX

```sh
bash scripts/build_visrtx.sh
```

The RTX device only (`-DVISRTX_BUILD_GL_DEVICE=OFF`), with the OptiX headers of barney, as on
Windows. `VISRTX_MIN_ARCH` stays at 75. With 120, the compute capability of the GPUs, the
build fails: for that target the nvcc of CUDA 13.3 runs `ptxas` over the PTX of the OptiX
programs, and `ptxas` stops at the OptiX intrinsics (`Call to '_optix_get_launch_index_x'
requires call prototype`). With 75, nvcc only writes the PTX, and OptiX compiles it for the
GPU at run time.

### 4.7 Visionaray

```sh
bash scripts/build_visionaray.sh
```

The script installs the Visionaray headers (with CUDA, without viewer, examples and the
`common` library, as on Windows) and builds anari-visionaray with both devices, one library
each: `libanari_library_visionaray.so` (CPU) and `libanari_library_visionaray_cuda.so`.
`ANARI_VISIONARAY_CUDA=OFF` builds the CPU device only.

The CPU device is compiled with `-fno-strict-aliasing` when the compiler is GCC
([section 10](#10-source-changes-made-for-linux)): without it, GCC 14 at `-O3` produces a
device which renders NaN pixels and wrong shading.

### 4.8 anari-ospray

```sh
bash scripts/build_anari_ospray.sh
```

The script downloads `ospray-3.2.0.x86_64.linux.tar.gz` from the OSPRay releases to
`external/ospray` if it is not there, builds the device against it and installs it to
`install/ospray/lib`. It then copies the OSPRay libraries of the release next to the device
(without the test, example, GUI and MPI libraries). The device finds them through its rpath
(`$ORIGIN`).

Inside Blender, the libraries which Blender has already loaded win over the ones of OSPRay
with the same name (`libembree4.so.4`, `libtbb.so.12`, `libOpenImageDenoise.so.2`), as on
Windows. On Linux that does not work for volumes, see
[section 7](#7-what-does-not-work).

### 4.9 mitsuba-anari

```sh
bash scripts/build_mitsuba_anari.sh
```

Configured like on Windows (`-DMITSUBA_ANARI_USE_SYSTEM_ANARI_SDK=ON
-DMITSUBA_ANARI_DEP_CONFIGS=Release -DBUILD_TESTING=OFF`), plus
`-DMITSUBA_ANARI_MI_EXTRA_CXX_FLAGS=-march=x86-64-v2` for the Embree inside the Mitsuba
build (the x86-64-v3 default of the compiler again; the repository's own
`scripts/build-cs-p12.sh` does the same). Mitsuba and Dr.Jit are built while the device is
configured, with all cores; the script confines that step to `JOBS` cores with `taskset`. It
takes about 10 minutes.

The variants are `scalar_rgb`, `llvm_ad_rgb` and `cuda_ad_rgb`:

* `cuda_ad_rgb` needs no CUDA toolkit: Dr.Jit loads `libcuda.so.1` and `libnvoptix.so.1` of
  the driver at run time.
* `llvm_ad_rgb` loads the LLVM of the system, `/usr/lib64/libLLVM.so.21.1`. `env.sh` sets
  `DRJIT_LIBLLVM_PATH` to it. Nothing is staged next to the device
  (`MITSUBA_ANARI_LLVM_RUNTIME` is for Windows).

The device, `libmitsuba.so`, the Dr.Jit libraries and Mitsuba's `plugins/` and `data/` are
installed to `install/mitsuba/lib`.

### 4.10 Photon

```sh
bash scripts/build_photon.sh            # kokkos, gpu and cpu
bash scripts/build_photon.sh gpu        # one of them
```

As on Windows, Kokkos 5.2.2 is cloned to `external/kokkos` and installed twice, and two
builds of the device are made, both with the library name `photon`:

| Build | Kokkos | Ray tracing back end | Installed to |
|---|---|---|---|
| GPU | Cuda (`Kokkos_ARCH_BLACKWELL120`, `Kokkos_ENABLE_COMPILE_AS_CMAKE_LANGUAGE`) and Serial | OptiX (the 9.0 headers of barney) | `install/photon/lib` |
| CPU | Threads and Serial | Embree 4.4.1 (Blender's) | `install/photon_cpu/lib` |

* The CPU build uses the Threads back end as on Windows, although OpenMP would work with
  GCC, so that the two platforms run the same code.
* Blender's precompiled Embree has no CMake package on Linux (`lib/linux_x64/embree` only
  holds the headers and `libembree4.so`). The script writes a small one to
  `build/embree-blender` and passes `-Dembree_DIR`. Outside of Blender the CPU device needs
  `build/blender/bin/lib` on `LD_LIBRARY_PATH` (`libembree4.so.4`, `libtbb.so.12`); nothing
  is copied next to the device.
* Open Image Denoise is off, and the tests and command line apps are not built, as on
  Windows.

### 4.11 RadeonProRenderANARI

```sh
bash scripts/build_rpr.sh
```

The script clones the RadeonProRender SDK 3.1.7.1 with its `hipbin` submodule to
`external/rpr` (1.9 GB) and builds the device against `RadeonProRender/binUbuntu20`. The
install step puts `libanari_library_rpr.so`, `libRadeonProRender64.so`, `libNorthstar64.so`
and the kernels (`hipbin/`) into `install/rpr/lib`; RPR writes its cache to
`install/rpr/lib/cache`.

On Linux Northstar uses the CUDA driver library (`libcuda.so.1`) directly; the CUDA 13 run
time which the Windows build needs (`cudart64_13.dll`) is not loaded. The script still links
the node's `libcudart.so.13` next to the device, which does no harm.

Northstar does not render on the Blackwell GPUs, see [section 7](#7-what-does-not-work).

### 4.12 MoonRay (openmoonray-anari)

```sh
bash scripts/build_moonray_deps.sh      # Boost, JsonCpp, Log4cplus, CppUnit, Random123, ISPC
bash scripts/build_moonray.sh           # scene_rdl2, mcrt_denoise, moonray, moonshine, anari_dso, device
bash scripts/build_moonray.sh device    # one step
```

MoonRay is built from the forks under `openmoonray-anari/external/openmoonray` (the nested
submodules `cmake_modules`, `moonray/scene_rdl2`, `moonray/mcrt_denoise`, `moonray/moonray`
and `moonray/moonshine`). As on Windows and macOS the projects are built standalone, one
after the other, not through the openmoonray superproject (which also wants arras, hydra
and USD), with Unix Makefiles as upstream's Linux build does; everything is installed to
`install/moonray`:

| Step | Builds |
|---|---|
| `scene_rdl2` | the scene description library and its tools (`rdl2_print`, ...) |
| `mcrt_denoise` | the denoiser, with Open Image Denoise only |
| `moonray` | the renderer, the standalone `moonray`, the shader DSOs it ships |
| `moonshine` | the shader library: the Dwa materials and the pattern maps |
| `anari_dso` | the RDL2 DSOs of the device repository (`dso/`: `AnariBumpNormalMap`) |
| `device` | `libanari_library_moonray.so` → `install/moonray/lib` |
| `tests [regex]` | only when named: the tests of the device repository, see below |

Upstream targets Rocky Linux 9 and installs its dependencies with `dnf` and a dependency
build under `building/Rocky9` (TBB 2020, OpenVDB 9, OpenImageIO 2.4, ...). Neither is used
here. The forks were ported forward to the current versions of these libraries for
Windows, and those are the versions of Blender's precompiled libraries, so MoonRay is
built against them. **Where each library comes from:**

| Library | Source | Why |
|---|---|---|
| TBB (oneTBB 2022.3) | Blender, `lib/linux_x64/tbb` | Blender has it loaded |
| Embree 4.4.1 | Blender, `lib/linux_x64/embree` | Blender has it loaded |
| OpenVDB 13 | Blender | Blender has it loaded (the choice of Windows, too) |
| OpenEXR 3.4, Imath | Blender | Blender has them loaded |
| OpenImageIO 3.1 | Blender | Blender has it loaded |
| OpenSubdiv 3.7 | Blender | Blender has it loaded |
| Open Image Denoise 2.5 | Blender | Blender has it loaded |
| libjpeg | Blender (static) | not installed on the node |
| Boost 1.86 (filesystem, program_options, regex, thread) | `install/moonray-deps` | Blender has no Boost |
| JsonCpp 1.9.5, Log4cplus 2.0.8, CppUnit 1.15.1 | `install/moonray-deps` | not in Blender |
| Random123 1.14 | `install/moonray-deps` (headers) | |
| ISPC 1.20.0 | `tools/`, the binary release | the compiler of MoonRay's ISPC code; no run time part |
| Lua 5.4 (with `luac`), OpenSSL, zlib, libatomic, flex, bison | the system | present on the node; where Blender loads one of them itself, it is the same system library |
| LLVM | not used | ISPC is a build tool, MoonRay does not link LLVM |

Inside Blender a shared library whose soname is already loaded is not loaded again, so
MoonRay gets Blender's copy of the first group whatever is installed next to it; building
against exactly those copies is what makes that safe. With the device loaded into Blender,
`/proc/self/maps` shows one copy of every library, all of the first group from
`build/blender/bin/lib`. The libraries of the second group have no counterpart in Blender
and are ordinary shared libraries under their own names.

What had to be provided for Blender's libraries:

* **Embree** has no CMake package in `lib/linux_x64` and no ISPC headers (`*.isph`;
  Blender builds it without ISPC support), which MoonRay's ISPC code includes.
  `build_moonray_deps.sh` takes the `.isph` files from the Embree 4.4.1 sources (they only
  declare the C API), and `env.sh` writes a package to `build/embree-blender` whose include
  directory holds links to Blender's headers and to these files. Photon's CPU build uses
  the same package.
* Blender's Embree is built with ray masks, which MoonRay needs, with back face culling
  for curves and spheres, and with AVX2 as its highest instruction set, which is the one
  MoonRay's ISPC code is compiled for (`avx2-i32x8`).
* `libembree4.so` depends on `libsycl.so`; the build has `build/blender/bin/lib` on
  `LD_LIBRARY_PATH` so that the linker resolves it.
* The OpenEXR package of Blender asks for `openjph`, which is added to the prefix path.

The installed libraries carry a run path to `install/moonray/lib`, `install/moonray-deps/lib`
and Blender's library directories (the device is configured with
`-DCMAKE_INSTALL_RPATH='$ORIGIN' -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON`; without a run
path it did not load into Blender: `librendering_rndr.so: cannot open shared object file`).
The device finds `rdl2dso/` next to its `lib/` directory.

The standalone renderer:

```sh
export LD_LIBRARY_PATH=install/moonray/lib:install/moonray-deps/lib:build/blender/bin/lib
export RDL2_DSO_PATH=install/moonray/rdl2dso REZ_MOONRAY_ROOT=install/moonray
install/moonray/bin/moonray -in openmoonray-anari/external/openmoonray/testdata/sphere.rdla -out sphere.exr
```

It renders upstream's `sphere.rdla`, `box.rdla` and `curves.rdla` (no NaN, not black).

**The tests of the device repository** (`openmoonray-anari/tests`, CTest) are built in a
tree of their own, `build/openmoonray-anari-tests`, and run from there; nothing is
installed:

```sh
bash scripts/build_moonray.sh tests               # the ANARI integration tests (label `integration`)
bash scripts/build_moonray.sh tests mesh_lights   # the tests whose name matches
```

The log is `build/logs/moonray-ctest.log`. With `MOONRAY_XPU=ON` the same is done against
the XPU build (`build/openmoonray-anari-xpu-tests`, `moonray-xpu-ctest.log`).

**XPU.** `MOONRAY_XPU=ON bash scripts/build_moonray.sh` makes a second, complete build
with MoonRay's GPU path (`-DMOONRAY_USE_OPTIX=YES`: CUDA 13.3 and OptiX) in
`build/moonray-xpu-*` and installs it to `install/moonray_xpu`.

* The OptiX 8.1 headers are used, the version of the Windows build; they are cloned from
  `github.com/NVIDIA/optix-dev` to `external/optix-8.1`. Building against the 9.0 headers
  of barney was not tried.
* `mcrt_denoise` is built without OptiX in both builds, as on Windows: its OptiX denoiser
  is written for OptiX 7 (`OptixDenoiserParams::denoiseAlpha`).
* The device selects the mode with the device parameter `moonray.executionMode=xpu`; the
  standalone renderer with `-exec_mode xpu`. Its log shows the GPU set up on the Blackwell
  GPU (`GPU: Loading .ptx: .../shaders/OptixGPUPrograms.ptx`, `... sm_120 ...`), and
  `nvidia-smi` lists the Blender process during an XPU smoke test.

### 4.13 pynari

```sh
bash scripts/build_pynari.sh
```

The script creates `build/pynari-venv` from the system Python 3.12 (Windows uses 3.13) with
numpy, pybind11, Pillow and matplotlib, and builds the module
`build/pynari/pynari.cpython-312-x86_64-linux-gnu.so` with the options of
[`pynari/doc/windows/README.md`](../../../pynari/doc/windows/README.md). mpi4py, which only
`sample06` uses, is installed from its PyPI wheel together with the `mpich` package, which
puts an MPI library into the virtual environment; no environment module is needed.

## 5. Running Blender, the smoke tests and pynari

```sh
bash scripts/blender.sh [blender arguments]
```

`blender.sh` starts `build/blender/bin/blender` so that neither Blender nor an ANARI device
writes to the home directory:

| Variable | Value | Covers |
|---|---|---|
| `BLENDER_USER_RESOURCES` | `cache/blender-user` | Blender's configuration, scripts, extensions |
| `HOME` | `cache/home` | `~/.nv`, `~/.drjit` of Mitsuba, ... |
| `XDG_CACHE_HOME`, `XDG_CONFIG_HOME`, ... | `cache/xdg/...` | the kernel cache of Cycles, fontconfig |
| `XDG_RUNTIME_DIR` | `cache/xdg/runtime` | no access to the sockets of the login session (audio, D-Bus) |
| `CUDA_CACHE_PATH`, `OPTIX_CACHE_PATH` | `cache/nv/ComputeCache`, `cache/optix` | the JIT cache of CUDA, the program cache of OptiX (default `/var/tmp/OptixCache_<user>`) |
| `TMPDIR` | `cache/tmp` | temporary files |
| `CUDA_VISIBLE_DEVICES` | `0` (`ANARI_GPU`) | a test run only sees one of the two GPUs; `ANARI_GPU=all` lifts it |

The library search paths of the ANARI add-on are separated by `;` on Linux as well, and they
hold `libanari_library_<name>.so`: `install/lib`, and the directory of each device which is
installed on its own (`install/ospray/lib`, `install/mitsuba/lib`, `install/rpr/lib`,
one of `install/photon/lib` and `install/photon_cpu/lib`, and one of `install/moonray/lib`
and `install/moonray_xpu/lib`).

The smoke tests of [README section 4](README.md#4-smoke-tests):

```sh
bash scripts/run_smoke.sh                 # every configuration whose library is installed
bash scripts/run_smoke.sh cycles cycles_optix cycles_cuda helide
bash scripts/run_smoke.sh helide -- AnariSmokeTest.test_lights
python3 scripts/smoke_tables.py           # the tables of section 8 from the logs
```

Logs are `build/logs/smoke-<configuration>.txt`, images `build/test_out/<configuration>`.
A run which hangs is stopped after `SMOKE_TIMEOUT` seconds (3600). When Blender crashes,
the log is kept as `smoke-<configuration>-crash.txt` and the suite is run again without the
test which crashed. The configurations are those of [WINDOWS.md](WINDOWS.md#6-smoke-test-results) plus `cycles_cuda`
(`computeDevice=cuda`), `rpr_cpu` (`computeDevice=cpu`), `mitsuba_llvm`
(`mitsuba.variant=llvm_ad_rgb`) and `moonray_xpu` (`moonray.executionMode=xpu` with the
libraries of `install/moonray_xpu`).

The pynari samples:

```sh
bash scripts/run_pynari.sh                          # every installed configuration
bash scripts/run_pynari.sh -r cycles_optix,cycles_cuda -j 1 --results build/pynari-out/results-gpu.json
python3 scripts/pynari_table.py                     # the table of section 9
```

`run_pynari.py` is `pynari/doc/windows/tools/run_all.py` with the paths and configurations
of this workspace: the samples, the checks and the result files are the same. Runs which
execute at the same time need different `--results` files. Device libraries and the shared
libraries they depend on are found through `LD_LIBRARY_PATH`, which the script sets for
each configuration.

## 6. Differences from Windows

* **Libraries are in `lib`.** Windows installs DLLs to `install/bin`; on Linux the device
  libraries are `install/lib/libanari_library_<name>.so`. `common.cmake` sets
  `CMAKE_INSTALL_LIBDIR=lib`, otherwise Rocky Linux would use `lib64`.
* **Blender needs the install step**, see [4.3](#43-blender).
* **The compiler targets x86-64-v3.** The GCC of Rocky Linux 10 enables AVX2 and FMA without
  any flag. A project which builds code for a lower instruction set by *adding* flags
  (`-msse4.2`) still gets AVX2. helide's Embree is such a case: it is configured for SSE4.2
  only, its sources see `__AVX__` and reference the 8-wide primitives of the AVX libraries,
  which are not built. The link of a shared library does not complain, and the device then
  fails to load (`undefined symbol: _ZNK6embree7CurveNiILi8EE4Type4nameEv`). The SDK and the
  Mitsuba build are therefore built with `-march=x86-64-v2`. Blender sets
  `-march=x86-64-v2` itself.
* **GCC applies strict aliasing.** anari-visionaray's CPU device depends on type punning
  which MSVC and Clang let pass, see [4.7](#47-visionaray).
* **CUDA 13.3 instead of 12.8**, the only toolkit on the node which can be used
  ([4.1](#41-tools-and-compilers)). Two places needed care: the PTX of OptiX programs must
  not be generated for `sm_120` ([4.6](#46-visrtx)), and `CUDAToolkit_INCLUDE_DIRS` is a
  list of two directories in CUDA 13 (Photon, [section 10](#10-source-changes-made-for-linux)).
* **Blender is headless**, without the OpenGL back end and without OpenAL
  ([4.3](#43-blender)).
* **One library per name in a process.** On Windows a device directory can hold its own
  copy of a DLL; on Linux a shared library whose soname is already loaded is not loaded a
  second time, and symbols are searched in the libraries of the executable first. A device
  inside Blender therefore always gets Blender's `libembree4.so.4`, `libtbb.so.12`,
  `libOpenImageDenoise.so.2`, `libOpenImageIO.so.3.1`, ... whatever is installed next to it.
  cyclesphi-anari, Photon and MoonRay are built for exactly that; for OSPRay it is a problem
  ([section 7](#7-what-does-not-work)).
* **Symbols of Blender are hidden.** Blender for Linux is linked with a version script
  which only exports the Python and the memory allocation symbols, so a device which
  contains its own Cycles (cyclesphi-anari) does not get mixed with Blender's, unlike on
  macOS ([MACOS.md, section 6](MACOS.md#6-differences-from-windows)).
* **Line endings.** The Linux mirrors are LF check-outs.

## 7. What does not work

* **RadeonProRender on the GPU.** The precompiled CUDA kernels of the RadeonProRender SDK
  3.1.7.1 (`hipbin/*.cudabin`, the newest release and the newest kernels) are built for
  `sm_75`, `sm_80`, `sm_86`, `sm_89` and `sm_90`. There are none for Blackwell (`sm_120`).
  Northstar then creates its context without any GPU (`RPR_CONTEXT_GPU0_NAME` is empty) and
  every frame is black. The OpenCL back end (`computeDevice=opencl`) finds the GPU but
  fails in `rprContextRender` (RPR error -18). The device now notices the context without a
  GPU and falls back to the CPU with a warning
  ([section 10](#10-source-changes-made-for-linux)), so `rpr` renders on the CPU here.
  This is a limit of the SDK on this GPU generation, not of Linux.
* **RadeonProRender on the CPU** crashes (segmentation fault inside `rprContextRender`) in
  three pynari samples, the scenes of several hundred sphere instances: `sample02`,
  `viewer-sample02` and `interactive-bobblespheres3`. The other 33 samples and the smoke
  tests pass. It is slow: `sample03` takes 6.5 minutes.
* **OSPRay volumes inside Blender.** The volume of `test_object_types` crashes Blender in
  `Volume_intersect_kernel` of `libospray_module_cpu.so`. Inside Blender, OSPRay 3.2.0
  (built for Embree 4.3.3) runs on Blender's Embree 4.4.1. The same crash happens outside
  of Blender when Blender's `libembree4.so.4` is preloaded into a pynari sample with a
  volume (`sample03`), and not with OSPRay's own Embree: all 36 pynari samples pass. On
  Windows the same combination works. A likely reason for the difference: Blender's Embree
  is compiled for instruction sets up to AVX2, OSPRay's own up to AVX-512, and on this
  CPU OSPRay's ISPC code runs its AVX-512 variant (the crash is in
  `Volume_intersect_kernel...avx512skx`), while the Windows machine only has AVX2. The
  other 17 smoke tests pass. A fix needs an OSPRay which is built against the Embree of
  Blender.
* **Mitsuba `llvm_ad_rgb`, volumes from Blender.** The volume of `test_object_types` is
  not rendered: `render failed: jit_var_gather(): attempted to gather from an empty
  array!`. `cuda_ad_rgb` renders it, and the volumes of the pynari samples work with both
  variants. There is no Windows smoke test log of this variant to compare with.
* **Mitsuba, `test_emissive_room`** (new on 2026-10-11): the room which is only lit by an
  emissive mesh stays black with both variants (`the lamp does not light the room`, mean
  0.0). There is no Windows log of the suite with this test for mitsuba, so whether this
  is a difference of the platform is not known.
* **helide, `test_light_falloff`** (new on 2026-10-11) fails: the floor is 20 times
  brighter than in Cycles. helide is the minimal reference device, and there is no
  Windows log of this test for it either.
* **openmoonray-anari, `anari.hdri_light`.** Of the 46 ANARI integration tests of the
  device repository, this one fails: a constant environment of radiance 1 on an albedo of
  0.4 renders 0.4259 instead of 0.4 (the tolerance is 0.021); the value is the same in
  every run and with the XPU build. The environment tests of the smoke suite pass
  (brightness ratio 0.995). It was not looked into; the world and the environment light
  of the device were being reworked when this was tested.
* **The module `CUDA/12.9.1`** cannot be used, see [4.1](#41-tools-and-compilers).
* **A Blender with a window** needs the X11 or Wayland and EGL development packages, which
  are not installed. The OpenGL back end is off for the same reason.
* **Audio.** With OpenAL built in, a render in background mode loaded `libpulse.so.0` and
  connected to the PulseAudio socket of the login session, although the tests pass
  `-noaudio`. systemd then started `pipewire`, `pipewire-pulse`, `wireplumber` and the D-Bus
  broker for the user, and wireplumber wrote a state file below `~/.local/state`.
  Two things prevent it now: Blender is built without OpenAL, and `env.sh` gives every
  process a runtime directory of its own (`XDG_RUNTIME_DIR=cache/xdg/runtime`), no session
  bus address and `ALSOFT_DRIVERS=null`, so the sockets of the session cannot be reached.
* **Core dumps.** The node hands core files to `systemd-coredump`. `env.sh` sets
  `ulimit -c 0`, so a crashing test leaves nothing outside of the workspace.
* **strace** is not permitted on the node (`ptrace: Operation not permitted`);
  `LD_DEBUG=files` shows which libraries a process loads.

## 8. Smoke test results

Run of 2026-10-11 (late morning) on this node, 160×120 at 32 samples: the suite of 18
tests (`test_emissive_room` and `test_light_falloff` are new), Blender `ba0634dac1ff`,
openmoonray-anari `a4fa3a77107d` with the moonray fork at `75116ce453f0` (the MeshLight
changes), all configurations in one go, one after the other; the GPU configurations on
one RTX PRO 6000 Blackwell.

* **rpr (default)** is the device without parameters: it falls back to the CPU here, so the
  column equals **rpr (CPU)**.
* **ospray:** the suite was run without `test_object_types`, which crashes
  ([section 7](#7-what-does-not-work)).

### Comparison with Windows

The numbers were compared with the Windows logs `build/logs/smoke-final-<name>.txt`; the
table says which log each row uses. Only the logs of cycles (CPU), barney and moonray are
from the same morning and have the two new tests. The others are older, some by two
days, and the exporter changed in between (normal maps, the shader graph materials), so
the comparison is split: the metrics of the tables in WINDOWS.md, which did not
change, and the rest.

| Linux | Windows log | table metrics: lines, largest difference | other metrics which differ |
|---|---|---|---|
| cycles | cycles, 10-11 09:25 | 24, 0.002 | - |
| cycles_optix | cycles_optix, 10-11 02:37 | 22, 0.002 | - |
| cycles_cuda | cycles_optix, 10-11 02:37 | 22, 0.002 | device denoiser 0.030 |
| helide | helide, 10-10 10:06 | 22, 0.000 | shader graph mix_shader 0.079 |
| barney | barney, 10-11 10:11 | 24, 0.001 | device denoiser 0.060 |
| visrtx | visrtx, 10-10 10:02 | 22, 0.001 | shader graph mix_shader 0.523 |
| visrtx_quality | visrtx_quality, 10-11 01:54 | 22, 0.001 | - |
| visionaray | visionaray, 10-09 22:58 | 22, 0.017 | normal map brightness ratio, 0.663; shader graph checker 2.420; shader graph noise_ramp 1.334; shader graph mix_shader 0.505 |
| visionaray_cuda | visionaray_cuda, 10-10 22:18 | 22, 0.004 | - |
| ospray | ospray, 10-10 10:06 | 10, 0.002 | shader graph mix_shader 0.520 |
| rpr | rpr, 10-09 23:04 | 22, 0.036 (lens shift relative difference, to the unshifted image) | shader graph noise_ramp 1.314 |
| rpr_cpu | rpr, 10-09 23:04 | 22, 0.036 (lens shift relative difference, to the unshifted image) | shader graph noise_ramp 1.314 |
| photon | photon, 10-10 10:07 | 22, 0.001 | normal map brightness ratio, 0.643; shader graph mix_shader 0.523 |
| photon_cpu | photon_cpu, 10-09 23:04 | 22, 0.051 (object gn_volume) | normal map brightness ratio, 0.643; shader graph checker 2.381; shader graph noise_ramp 1.312; shader graph mix_shader 0.523 |
| mitsuba | mitsuba, 10-10 10:05 | 22, 0.002 | shader graph mix_shader 0.418 |
| mitsuba_llvm | mitsuba, 10-10 10:05 | 22, 1.018 (object gn_volume) | shader graph mix_shader 0.418 |
| moonray | moonray, 10-11 10:13 | 24, 0.012 | device denoiser 0.020; shader graph noise_ramp 0.235 |
| moonray_xpu | moonray, 10-11 10:13 | 24, 0.012 | device denoiser 0.020; shader graph noise_ramp 0.235 |

* The metrics of the tables in WINDOWS.md agree with Windows within 0.004 for every device, except:
  * visionaray on the CPU, 0.017: its images are noisier, as on the Mac.
  * rpr, 0.036 in one number of the lens shift test: Windows rendered on the GPU, Linux on
    the CPU.
  * photon (CPU), 0.051 for the volume: the Windows log of this configuration is from
    2026-10-09; against the Windows log of the GPU build of 2026-10-10 the difference is 0.
  * mitsuba (llvm): the volume which is not rendered, see
    [section 7](#7-what-does-not-work). It is compared with the Windows log of
    `cuda_ad_rgb`.
* Where the Windows log is from the same day (cycles, barney, visrtx with the `quality`
  renderer, moonray) or the evening before (visionaray with CUDA), all other metrics agree
  as well, apart from what is said about moonray below, and except the
  noise of the device denoiser of barney (ratio 0.50 on Windows, 0.44 here; not looked into).
  cycles with CUDA is compared with the Windows log of OptiX and has another device denoiser
  (Open Image Denoise instead of the OptiX denoiser).
* The remaining differences of the shader graph and normal map tests all belong to Windows
  logs from before the evening of 2026-10-10. They are taken to be changes of the exporter
  between the runs, not differences of the platform; the tests were not run again on
  Windows to prove it.
* **moonray:** all 18 tests pass, as on Windows. The metrics of the tables are within 0.012
  of the Windows log of the same morning (0.006 without the lens shift); MoonRay's images
  are noisier at 32 samples than the differences between the platforms. The two new
  tests: emissive room 0.981 / 0.014 (Windows 0.981 / 0.014), light falloff 1.022 / 0.033
  (Windows 1.016 / 0.033). One printed number differs more: the procedural noise material
  of `test_shader_graph` (relative difference to Cycles 1.347, Windows 1.112). Both are far
  from Cycles, whose noise MoonRay does not reproduce; why the two platforms differ was
  not looked into.
  **moonray (XPU)** gives the same numbers as the CPU build to three decimals: XPU moves
  the ray intersections to the GPU, the shading is the same code. It takes 265 s instead
  of 24 s for the suite, because every render sets up the GPU first.
* **MoonRay's mesh lights.** The changes of the fork in `MeshLight.cc` and
  `MeshLight.ispc` compile for the ISPC target of the Linux build (`avx2-i32x8`) in the
  CPU and in the XPU build. The mesh light tests of the device repository
  (`anari.mesh_lights_*`, 11 tests) pass with both builds.
* **barney:** all 18 tests pass, as on Windows, with the numbers of the Windows log
  (within 0.001), the two new tests included.
* **cyclesphi-anari** with the kernel changes of the Metal port: CPU, OptiX and CUDA give
  the same metrics, within 0.002 of Windows. The Linux build never existed without those
  changes (they were part of the first sync), so there are no Linux numbers from before.
* **helide** fails `test_environment_map` (both rotations) and `test_camera_clip` as on
  Windows: it has no `hdri` light and no clip planes. It also fails the new
  `test_light_falloff`, and **mitsuba** the new `test_emissive_room`
  ([section 7](#7-what-does-not-work)); the Windows logs of these two devices are older
  than the tests.

### Test status

| | cycles (CPU) | cycles (OptiX) | cycles (CUDA) | helide | barney | visrtx (default) | visrtx (quality) | visionaray (CPU) | visionaray (CUDA) | ospray | rpr (default) | rpr (CPU) | photon (CUDA) | photon (CPU) | mitsuba (cuda) | mitsuba (llvm) | moonray (CPU) | moonray (XPU) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `test_background` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_basic_render` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_build_options` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_camera_clip` | pass | pass | pass | **fail** | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_camera_shift` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_device_denoiser` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_emissive_room` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | **fail** | **fail** | pass | pass |
| `test_engine_registered` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_environment_map` | pass | pass | pass | **fail** | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_instancing` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_light_falloff` | pass | pass | pass | **fail** | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_lights` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_normal_map` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_object_types` | pass | pass | pass | pass | pass | pass | pass | pass | pass | – | pass | pass | pass | pass | pass | **fail** | pass | pass |
| `test_shader_graph` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_texture` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_texture_mapping` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| `test_world_camera_ray` | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass | pass |
| result | 18 tests, 3.174 s, OK | 18 tests, 10.113 s, OK | 18 tests, 10.665 s, OK | 18 tests, 2.693 s, FAILED (failures=4) | 18 tests, 10.685 s, OK | 18 tests, 7.504 s, OK | 18 tests, 7.563 s, OK | 18 tests, 6.700 s, OK | 18 tests, 3.602 s, OK | 17 tests, 83.936 s, OK | 18 tests, 81.759 s, OK | 18 tests, 81.207 s, OK | 18 tests, 5.785 s, OK | 18 tests, 4.797 s, OK | 18 tests, 13.131 s, FAILED (failures=1) | 18 tests, 25.623 s, FAILED (failures=2) | 18 tests, 24.260 s, OK | 18 tests, 264.888 s, OK |

### Brightness ratio and relative difference against Blender Cycles

Cells are *brightness ratio / relative difference*; for texture mapping the pattern agreement, for instancing and the lens shift the relative difference.

| | cycles (CPU) | cycles (OptiX) | cycles (CUDA) | helide | barney | visrtx (default) | visrtx (quality) | visionaray (CPU) | visionaray (CUDA) | ospray | rpr (default) | rpr (CPU) | photon (CUDA) | photon (CPU) | mitsuba (cuda) | mitsuba (llvm) | moonray (CPU) | moonray (XPU) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| basic render | 0.999 / 0.020 | 0.999 / 0.020 | 0.999 / 0.020 | 0.518 / 0.819 | 1.011 / 0.412 | 1.011 / 0.309 | 1.043 / 0.042 | 1.135 / 0.336 | 1.134 / 0.347 | 1.024 / 0.452 | 1.043 / 0.085 | 1.043 / 0.085 | 1.018 / 0.076 | 1.018 / 0.076 | 1.050 / 0.191 | 1.050 / 0.191 | 1.051 / 0.063 | 1.051 / 0.063 |
| texture mapping | 1.000 | 1.000 | 1.000 | 0.944 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 | 1.000 |
| point light | 1.000 / 0.000 | 1.000 / 0.000 | 1.000 / 0.000 | 1.303 / 0.469 | 1.031 / 0.049 | 1.033 / 0.051 | 1.033 / 0.051 | 1.097 / 0.117 | 1.097 / 0.117 | 0.941 / 0.169 | 1.064 / 0.088 | 1.064 / 0.088 | 0.999 / 0.051 | 0.999 / 0.051 | 1.111 / 0.123 | 1.111 / 0.123 | 1.085 / 0.097 | 1.085 / 0.097 |
| spot light | 1.000 / 0.009 | 1.000 / 0.009 | 1.000 / 0.009 | 4.664 / 1.665 | 1.034 / 0.499 | 1.034 / 0.501 | 1.034 / 0.501 | 1.108 / 0.583 | 1.108 / 0.582 | 0.944 / 0.074 | 1.021 / 0.490 | 1.021 / 0.490 | 1.011 / 0.478 | 1.011 / 0.478 | 1.123 / 0.596 | 1.123 / 0.596 | 1.068 / 0.074 | 1.068 / 0.074 |
| area light | 1.000 / 0.000 | 1.000 / 0.000 | 1.000 / 0.000 | 0.389 / 0.725 | 0.966 / 0.039 | 0.966 / 0.039 | 0.967 / 0.039 | 1.021 / 0.036 | 1.021 / 0.036 | 0.875 / 0.186 | 0.998 / 0.036 | 0.998 / 0.036 | 0.966 / 0.039 | 0.966 / 0.039 | 1.041 / 0.046 | 1.041 / 0.046 | 1.005 / 0.007 | 1.005 / 0.007 |
| sun light | 1.000 / 0.001 | 1.000 / 0.001 | 1.000 / 0.001 | 0.953 / 0.065 | 0.974 / 0.044 | 0.974 / 0.043 | 0.974 / 0.043 | 0.971 / 0.034 | 0.971 / 0.034 | 1.113 / 0.113 | 0.971 / 0.036 | 0.971 / 0.036 | 0.974 / 0.043 | 0.974 / 0.043 | 1.043 / 0.046 | 1.043 / 0.046 | 1.007 / 0.008 | 1.007 / 0.008 |
| environment 0 deg | 1.000 / 0.002 | 1.000 / 0.002 | 1.000 / 0.002 | 2.241 / 1.666 | 0.993 / 0.021 | 0.993 / 0.021 | 0.993 / 0.021 | 0.988 / 0.047 | 0.988 / 0.047 | 0.997 / 0.040 | 0.996 / 0.012 | 0.996 / 0.012 | 0.993 / 0.031 | 0.993 / 0.031 | 0.999 / 0.005 | 0.999 / 0.005 | 0.995 / 0.017 | 0.995 / 0.017 |
| environment 90 deg | 1.000 / 0.001 | 1.000 / 0.001 | 1.000 / 0.001 | 2.332 / 1.737 | 0.994 / 0.020 | 0.994 / 0.019 | 0.994 / 0.019 | 0.989 / 0.043 | 0.989 / 0.043 | 0.984 / 0.052 | 0.997 / 0.010 | 0.997 / 0.010 | 0.985 / 0.034 | 0.985 / 0.034 | 0.998 / 0.006 | 0.998 / 0.006 | 0.996 / 0.012 | 0.996 / 0.012 |
| instancing | 0.001 | 0.001 | 0.001 | 0.041 | 0.021 | 0.021 | 0.021 | 0.009 | 0.009 | 0.039 | 0.002 | 0.002 | 0.022 | 0.022 | 0.008 | 0.008 | 0.007 | 0.007 |
| lens shift | 0.014 | 0.014 | 0.014 | 0.816 | 0.457 | 0.305 | 0.072 | 0.359 | 0.360 | 0.437 | 0.092 | 0.092 | 0.077 | 0.077 | 0.181 | 0.181 | 0.047 | 0.047 |
| clip start | 0.999 / 0.018 | 0.999 / 0.018 | 0.999 / 0.018 | 0.000 / 1.000 | 1.010 / 0.415 | 1.003 / 0.288 | 1.042 / 0.037 | 1.156 / 0.351 | 1.159 / 0.351 | 1.033 / 0.430 | 1.007 / 0.079 | 1.007 / 0.079 | 1.010 / 0.069 | 1.010 / 0.069 | 1.014 / 0.187 | 1.014 / 0.187 | 1.051 / 0.047 | 1.051 / 0.047 |
| normal map | 1.000 / 0.001 | 1.000 / 0.001 | 1.000 / 0.001 | 2.356 / 1.201 | 1.050 / 0.052 | 1.050 / 0.052 | 1.050 / 0.052 | 1.030 / 0.032 | 1.031 / 0.034 | 1.047 / 0.047 | 1.053 / 0.051 | 1.053 / 0.051 | 1.050 / 0.052 | 1.050 / 0.052 | 1.143 / 0.695 | 1.143 / 0.695 | 1.051 / 0.049 | 1.051 / 0.049 |
| world for camera rays | 1.000 / 0.004 | 1.000 / 0.004 | 1.000 / 0.004 | 1.001 / 0.318 | 0.978 / 0.053 | 0.976 / 0.052 | 0.976 / 0.052 | 0.952 / 0.116 | 0.953 / 0.116 | 0.989 / 0.099 | 0.987 / 0.029 | 0.987 / 0.029 | 0.975 / 0.078 | 0.975 / 0.078 | 0.993 / 0.019 | 0.993 / 0.019 | 0.987 / 0.030 | 0.987 / 0.030 |
| emissive room | 1.001 / 0.006 | 1.001 / 0.006 | 1.001 / 0.006 | 0.359 / 0.935 | 0.922 / 0.051 | 0.624 / 0.204 | 0.932 / 0.038 | 1.021 / 0.017 | 1.020 / 0.016 | 0.708 / 0.443 | 0.345 / 0.899 | 0.345 / 0.899 | 0.933 / 0.041 | 0.933 / 0.041 | – | – | 0.981 / 0.014 | 0.981 / 0.014 |
| light falloff | 0.999 / 0.009 | 0.999 / 0.009 | 0.999 / 0.009 | 20.061 / 4.909 | 0.347 / 0.844 | 0.347 / 0.844 | 0.347 / 0.844 | 0.355 / 0.843 | 0.355 / 0.843 | 0.315 / 0.867 | 0.348 / 0.843 | 0.348 / 0.843 | 0.347 / 0.843 | 0.347 / 0.843 | 0.347 / 0.844 | 0.347 / 0.844 | 1.022 / 0.033 | 1.022 / 0.033 |

### Object types

Cells are *mask coverage / brightness ratio / relative difference*.

| | cycles (CPU) | cycles (OptiX) | cycles (CUDA) | helide | barney | visrtx (default) | visrtx (quality) | visionaray (CPU) | visionaray (CUDA) | ospray | rpr (default) | rpr (CPU) | photon (CUDA) | photon (CPU) | mitsuba (cuda) | mitsuba (llvm) | moonray (CPU) | moonray (XPU) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| gn_instances | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.026 / 0.118 | 1.000 / 1.001 / 0.023 | 1.000 / 0.991 / 0.032 | 1.000 / 0.995 / 0.022 | 1.000 / 1.003 / 0.012 | 1.000 / 1.003 / 0.012 | – | 1.000 / 1.001 / 0.004 | 1.000 / 1.001 / 0.004 | 1.000 / 0.994 / 0.025 | 1.000 / 0.994 / 0.025 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | 1.000 / 0.999 / 0.003 | 1.000 / 0.999 / 0.003 |
| gn_realized | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.028 / 0.133 | 1.000 / 1.001 / 0.023 | 1.000 / 0.991 / 0.032 | 1.000 / 0.995 / 0.022 | 1.000 / 1.003 / 0.012 | 1.000 / 1.003 / 0.012 | – | 1.000 / 1.001 / 0.004 | 1.000 / 1.001 / 0.004 | 1.000 / 0.994 / 0.025 | 1.000 / 0.994 / 0.025 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | 1.000 / 1.000 / 0.003 | 1.000 / 1.000 / 0.003 |
| gn_points | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.030 / 0.102 | 1.000 / 1.003 / 0.011 | 1.000 / 0.989 / 0.028 | 1.000 / 0.996 / 0.010 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | – | 1.000 / 1.001 / 0.003 | 1.000 / 1.001 / 0.003 | 1.000 / 0.995 / 0.015 | 1.000 / 0.995 / 0.015 | 1.000 / 1.001 / 0.008 | 1.000 / 1.001 / 0.008 | 1.000 / 1.000 / 0.003 | 1.000 / 1.000 / 0.003 |
| gn_curves | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 | 1.000 / 1.036 / 0.128 | 1.000 / 1.005 / 0.016 | 1.000 / 0.987 / 0.035 | 1.000 / 0.997 / 0.011 | 0.987 / 0.999 / 0.016 | 0.987 / 0.999 / 0.016 | – | 1.000 / 1.000 / 0.004 | 1.000 / 1.000 / 0.004 | 1.000 / 0.998 / 0.010 | 1.000 / 0.998 / 0.010 | 1.000 / 1.000 / 0.007 | 1.000 / 1.000 / 0.007 | 1.000 / 1.022 / 0.077 | 1.000 / 1.022 / 0.076 |
| gn_volume | 0.984 / 1.000 / 0.003 | 0.984 / 1.000 / 0.003 | 0.984 / 1.000 / 0.003 | 0.992 / 1.121 / 0.411 | 1.000 / 0.983 / 0.056 | 0.997 / 1.121 / 0.411 | 0.998 / 0.999 / 0.006 | 0.999 / 0.984 / 0.056 | 0.998 / 0.984 / 0.055 | – | 0.997 / 1.020 / 0.053 | 0.997 / 1.020 / 0.053 | 1.000 / 1.000 / 0.004 | 1.000 / 1.000 / 0.004 | 0.999 / 1.018 / 0.053 | 0.102 / 0.000 / 1.000 | 1.000 / 1.001 / 0.005 | 1.000 / 1.001 / 0.005 |
| particle_instances | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.012 / 0.078 | 1.000 / 1.000 / 0.004 | 1.000 / 0.995 / 0.028 | 1.000 / 0.998 / 0.010 | 1.000 / 1.000 / 0.003 | 1.000 / 1.000 / 0.003 | – | 1.000 / 1.001 / 0.003 | 1.000 / 1.001 / 0.003 | 1.000 / 0.997 / 0.019 | 1.000 / 0.997 / 0.019 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 |
| particle_hair | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | 1.000 / 1.001 / 0.006 | 0.915 / 1.009 / 0.047 | 0.999 / 1.001 / 0.008 | 0.999 / 0.990 / 0.039 | 1.000 / 0.999 / 0.007 | 1.000 / 1.001 / 0.006 | 0.997 / 1.001 / 0.007 | – | 0.999 / 1.002 / 0.005 | 0.999 / 1.002 / 0.005 | 0.997 / 0.996 / 0.014 | 0.997 / 0.996 / 0.014 | 0.997 / 0.995 / 0.020 | 0.997 / 0.997 / 0.015 | 0.999 / 1.006 / 0.027 | 0.999 / 1.006 / 0.027 |
| hair_curves | 1.000 / 1.002 / 0.021 | 1.000 / 1.002 / 0.021 | 1.000 / 1.002 / 0.021 | 1.000 / 1.060 / 0.184 | 1.000 / 1.009 / 0.030 | 1.000 / 0.986 / 0.040 | 1.000 / 1.000 / 0.019 | 1.000 / 1.002 / 0.018 | 1.000 / 1.002 / 0.017 | – | 1.000 / 1.002 / 0.021 | 1.000 / 1.002 / 0.021 | 1.000 / 0.999 / 0.022 | 1.000 / 0.999 / 0.022 | 1.000 / 1.001 / 0.021 | 1.000 / 1.002 / 0.022 | 1.000 / 1.024 / 0.071 | 1.000 / 1.024 / 0.070 |
| text | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.009 / 0.043 | 1.000 / 1.001 / 0.006 | 1.000 / 0.998 / 0.009 | 1.000 / 0.998 / 0.007 | 1.000 / 1.000 / 0.005 | 1.000 / 1.000 / 0.005 | – | 1.000 / 1.002 / 0.002 | 1.000 / 1.002 / 0.002 | 1.000 / 0.998 / 0.010 | 1.000 / 0.998 / 0.010 | 1.000 / 1.000 / 0.005 | 1.000 / 1.000 / 0.005 | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |
| bevel_curve | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.022 / 0.080 | 1.000 / 1.004 / 0.016 | 1.000 / 0.996 / 0.011 | 1.000 / 0.997 / 0.008 | 1.000 / 1.000 / 0.011 | 1.000 / 0.999 / 0.010 | – | 1.000 / 1.000 / 0.004 | 1.000 / 1.000 / 0.004 | 1.000 / 0.997 / 0.012 | 1.000 / 0.997 / 0.012 | 1.000 / 1.000 / 0.007 | 1.000 / 1.000 / 0.007 | 1.000 / 0.999 / 0.004 | 1.000 / 0.999 / 0.004 |
| metaball | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.015 / 0.125 | 1.000 / 1.003 / 0.017 | 1.000 / 0.996 / 0.016 | 1.000 / 0.998 / 0.011 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | – | 1.000 / 1.001 / 0.003 | 1.000 / 1.001 / 0.003 | 1.000 / 0.998 / 0.012 | 1.000 / 0.998 / 0.012 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |
| subdivision | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.000 / 0.001 | 1.000 / 1.013 / 0.119 | 1.000 / 1.002 / 0.018 | 1.000 / 0.998 / 0.013 | 1.000 / 0.998 / 0.013 | 1.000 / 1.001 / 0.010 | 1.000 / 1.001 / 0.011 | – | 1.000 / 1.002 / 0.003 | 1.000 / 1.002 / 0.003 | 1.000 / 0.997 / 0.015 | 1.000 / 0.997 / 0.015 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.008 | 1.000 / 1.000 / 0.002 | 1.000 / 1.000 / 0.002 |

## 9. pynari results

Runs of 2026-10-10 and 2026-10-11 (moonray again after the MeshLight changes, with
pynari `69c6d87ed4a5`): the matrix of
[`pynari/doc/windows/README.md`](../../../pynari/doc/windows/README.md#5-results) (`cycles_cuda`
is not part of the Windows matrix). Each cell shows whether the
run passed (the process exited normally and wrote an image which is neither black nor flat)
and its time. The times include process start-up and are only a rough guide: CPU
configurations ran two to four jobs in parallel, next to other tests and builds.

* 537 of 540 runs pass; on Windows all 504 of its matrix pass.
* The three failures are `rpr` on the CPU, see [section 7](#7-what-does-not-work). On
  Windows `rpr` renders on the GPU.
* `mitsuba_cuda` and `mitsuba_llvm` ran with the fix of
  [section 10](#10-source-changes-made-for-linux).
* `moonray` passes all 36 samples. `sample03`, which takes 6.5 minutes on Windows, takes
  30 to 100 s on the 64 cores here, depending on what else runs.

| sample | helide | visrtx | barney | cycles_optix | cycles_cuda | cycles_cpu | mitsuba_cuda | mitsuba_llvm | moonray | visionaray | visionaray_cuda | ospray | rpr | photon | photon_cpu |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| camera-depth-of-field | ok 92s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 2s | ok 2s | ok 3s | ok 2s | ok 3s | ok 1s | ok 49s | ok 2s | ok 4s |
| geometry-cones | ok 0s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 1s | ok 2s | ok 1s | ok 11s | ok 1s | ok 2s |
| geometry-cones-colorPerPrim | ok 0s | ok 1s | ok 14s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 1s | ok 13s | ok 1s | ok 1s |
| geometry-cones-colorPerVertex | ok 0s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 13s | ok 1s | ok 2s |
| geometry-cylinders | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 1s | ok 11s | ok 2s | ok 2s |
| geometry-cylinders-colorPerPrim | ok 1s | ok 1s | ok 7s | ok 3s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 1s | ok 2s | ok 1s | ok 13s | ok 1s | ok 1s |
| geometry-cylinders-colorPerVertex | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 13s | ok 1s | ok 1s |
| geometry-spheres-with-sampler1D | ok 1s | ok 2s | ok 1s | ok 2s | ok 2s | ok 10s | ok 3s | ok 7s | ok 22s | ok 14s | ok 10s | ok 6s | ok 195s | ok 4s | ok 20s |
| geometry-triangles-testorb-np | ok 1s | ok 1s | ok 2s | ok 2s | ok 1s | ok 7s | ok 2s | ok 2s | ok 30s | ok 10s | ok 4s | ok 3s | ok 90s | ok 2s | ok 13s |
| interactive-bobblespheres3 | ok 2s | ok 2s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 3s | ok 3s | ok 2s | ok 2s | ok 1s | **FAIL** 24s | ok 2s | ok 3s |
| reference-hdri | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 2s | ok 2s | ok 9s | ok 3s | ok 2s | ok 1s | ok 37s | ok 2s | ok 4s |
| sample-getInfo | ok 1s | ok 0s | ok 1s | ok 1s | ok 0s | ok 0s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s |
| sample01 | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 2s | ok 2s | ok 4s | ok 2s | ok 1s | ok 1s | ok 46s | ok 2s | ok 4s |
| sample01_with_texture | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 2s | ok 1s | ok 4s | ok 2s | ok 1s | ok 1s | ok 42s | ok 2s | ok 3s |
| sample02 | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 3s | ok 1s | ok 5s | ok 3s | ok 2s | ok 2s | **FAIL** 23s | ok 2s | ok 4s |
| sample03 | ok 1s | ok 1s | ok 2s | ok 3s | ok 2s | ok 6s | ok 7s | ok 31s | ok 67s | ok 10s | ok 2s | ok 15s | ok 396s | ok 7s | ok 98s |
| sample03-isosurface | ok 1s | ok 2s | ok 2s | ok 2s | ok 1s | ok 2s | ok 2s | ok 2s | ok 7s | ok 12s | ok 2s | ok 4s | ok 415s | ok 2s | ok 20s |
| sample04 | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 4s | ok 2s | ok 3s | ok 4s | ok 4s | ok 3s | ok 2s | ok 34s | ok 4s | ok 6s |
| sample04_pointLight | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 2s | ok 3s | ok 4s | ok 4s | ok 3s | ok 2s | ok 44s | ok 4s | ok 6s |
| sample05 | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 3s | ok 2s | ok 7s | ok 2s | ok 2s | ok 2s | ok 129s | ok 3s | ok 5s |
| sample06 | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 4s | ok 1s | ok 1s | ok 1s | ok 18s | ok 2s | ok 2s |
| sample07 | ok 1s | ok 1s | ok 1s | ok 2s | ok 1s | ok 3s | ok 2s | ok 3s | ok 9s | ok 6s | ok 2s | ok 4s | ok 85s | ok 2s | ok 13s |
| sampler-image3d | ok 4s | ok 1s | ok 2s | ok 1s | ok 1s | ok 2s | ok 1s | ok 2s | ok 17s | ok 6s | ok 1s | ok 4s | ok 410s | ok 2s | ok 28s |
| sampler-image3d-borderColor | ok 1s | ok 1s | ok 2s | ok 1s | ok 1s | ok 3s | ok 1s | ok 2s | ok 26s | ok 5s | ok 2s | ok 3s | ok 428s | ok 2s | ok 18s |
| testing-release_00 | ok 1s | ok 0s | ok 7s | ok 0s | ok 0s | ok 0s | ok 0s | ok 0s | ok 1s | ok 1s | ok 1s | ok 0s | ok 1s | ok 0s | ok 0s |
| testing-release_01 | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s |
| testing-release_02 | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 1s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s |
| testing-release_03 | ok 0s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s |
| testing-release_04 | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 1s | ok 0s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s |
| testing-release_05 | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 0s | ok 0s |
| unstructured-cellCentric | ok 1s | ok 1s | ok 9s | ok 2s | ok 2s | ok 5s | ok 2s | ok 3s | ok 8s | ok 4s | ok 3s | ok 1s | ok 42s | ok 1s | ok 2s |
| unstructured-vertexCentric | ok 3s | ok 1s | ok 2s | ok 2s | ok 2s | ok 6s | ok 1s | ok 3s | ok 2s | ok 4s | ok 2s | ok 1s | ok 41s | ok 1s | ok 2s |
| viewer-sample01 | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 1s | ok 1s | ok 0s |
| viewer-sample02 | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 2s | ok 0s | ok 0s | ok 0s | **FAIL** 31s | ok 1s | ok 1s |
| viewer-sample05 | ok 0s | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 4s | ok 1s | ok 0s |
| viewer-sample07 | ok 1s | ok 1s | ok 1s | ok 1s | ok 1s | ok 0s | ok 1s | ok 1s | ok 1s | ok 0s | ok 0s | ok 0s | ok 2s | ok 1s | ok 1s |

537 of 540 runs passed.

`sample06` initializes MPI. The MPICH of the virtual environment would open the InfiniBand
ports of the node through UCX and fails there (`uct_iface_open(ud_verbs/mlx5_1:1) failed`),
so `run_pynari.py` sets `UCX_TLS=self,sm,tcp`.

## 10. Source changes made for Linux

All of them are working tree changes on Windows, like the rest.

**blenderphi**

* `source/creator/CMakeLists.txt`: the Linux branch installs the ANARI front-end library
  into `lib` next to the executable, under its soname (`libanari.so.0`). Only the Windows
  and the Apple branch installed it. The installed Blender links `libanari.so.0`, and its
  only rpath is `$ORIGIN/lib`; without the change it does not start
  (`libanari.so.0 => not found`).

* `.gitattributes`: `doc/anari/tools/linux/**` is checked out with LF line endings also
  where `core.autocrlf` is on. The scripts are copied to the Linux machine byte for byte
  and run by Git Bash; with CRLF they do not run.

**anari-visionaray**

* `CMakeLists.txt`: with GCC, the CPU device (`anari_library_visionaray`) is compiled with
  `-fno-strict-aliasing`. Built by GCC 14.3 at `-O3` without it, the device rendered NaN
  pixels and wrong shading: five smoke tests failed (for instance instancing with a relative
  difference of 0.294 instead of 0.009). `-O1` and `-fno-strict-aliasing` each make it
  render correctly; `-ffp-contract=off`, `-march=x86-64-v2`, `-fwrapv` and
  `-ftrivial-auto-var-init=zero` do not, and the result does not depend on the number of
  threads. The code which breaks the aliasing rules was not located
  (`-Wstrict-aliasing=2` only points at the debug print functions of `renderer/common.h`).
  The CUDA device is not affected.

**mitsuba-anari**

* `src/mitsuba_backend/MitsubaBackend.cpp`: on Linux, a process which used the CUDA variant
  shuts Dr.Jit down with `jit_shutdown(1)` (a light shutdown, which keeps the libraries of
  the driver loaded) when its last device is released. After the full shutdown, the next
  device crashed while it initialized OptiX again (right after `jit_optix_api_init(): loaded
  OptiX`). Blender creates a new device for every render, so the smoke tests crashed in
  their first test, at the second render. Other platforms and the LLVM variant are
  unchanged.

**photon**

* `src/CMakeLists.txt`: the command which compiles the OptiX PTX passes every directory of
  `CUDAToolkit_INCLUDE_DIRS` with its own `-I`. CUDA 13 has two (`include` and
  `include/cccl`), and the second one was taken for an input file (`nvcc fatal: A single
  input file is required`).
* `src/photon/pbrt/pbrt_parser.cpp`: `#include <cmath>`. The CPU build with GCC did not
  compile (`'sqrt' is not a member of 'std'`); the CUDA build gets the header through
  Kokkos.

**RadeonProRenderANARI**

* `RPRDevice.cpp`: on Linux, a GPU context whose GPU has no name is given up and the device
  falls back to the CPU with a warning, as it already does for a context which cannot be
  created. Northstar returns such a context when it has no precompiled kernels for the GPU
  ([section 7](#7-what-does-not-work)); every frame was black.

**openmoonray-anari** (and the MoonRay forks under `external/openmoonray`)

* `external/openmoonray/moonray/scene_rdl2/lib/common/grid_util/AffinityResourceControl.cc`:
  `#include <algorithm>`. GCC 14 no longer gets `std::sort` and `std::max_element` through
  the other headers. This is the only change the MoonRay forks needed: scene_rdl2,
  mcrt_denoise, moonray and moonshine otherwise compile with GCC 14 against Blender's
  libraries as they are.
* `tests/unit/test_sincos.cpp`: the two calls are written with their namespace
  (`openmoonray_anari::portable::sincos`). glibc declares `::sincos` and `::sincosf`
  itself, so the unqualified calls after `using namespace` were ambiguous and the tests
  of the device did not build on Linux.
* `src/anari_library_moonray/backend/MoonrayScene.h`: includes
  `<scene_rdl2/render/util/AtomicFloat.h>` before its `std::atomic<float>` member (Linux;
  the macOS port extended the guard to Apple). scene_rdl2 specializes `std::atomic<float>`
  for standard libraries without the C++20 floating point atomics, and GCC rejects a
  specialization which comes after the first use (`specialization of 'std::atomic<float>'
  after instantiation`).

The ANARI device of Blender (`intern/cycles/device/anari`), ANARI-SDK, cyclesphi-anari,
barney, VisRTX, visionaray, anari-ospray and pynari built with GCC 14 and CUDA 13.3 without
changes.
