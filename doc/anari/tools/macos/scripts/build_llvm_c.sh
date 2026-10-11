#!/usr/bin/env bash
# build_llvm_c.sh - an LLVM shared library which only exports the LLVM C API, for the
# llvm_ad_rgb variant of Mitsuba (Dr.Jit loads LLVM at runtime through its C API).
#
# Why not the libLLVM.dylib of Homebrew: it exports all C++ symbols of LLVM. Blender
# contains an LLVM of its own (inside liboslexec.dylib), and dyld binds the exported weak
# symbols (templates, inline functions) of all loaded images to one definition. The pass
# manager of one LLVM then runs on the data structures of the other, and the first kernel
# which Dr.Jit compiles inside Blender crashes. Windows does not have the problem: the
# LLVM-C.dll used there only exports the C API, and DLLs do not share symbols.
#
# The library is linked from the static LLVM libraries of Blender's precompiled libraries
# (blenderphi/lib/macos_arm64/llvm) with every object loaded and only `LLVM*` exported.
# Loading every object also pulls in the parts of LLVM which need libxml2 and libedit;
# the ones of macOS are linked.
# Result: install/llvm-c/lib/libLLVM.dylib. build_mitsuba_anari.sh installs it next to the
# device, where Dr.Jit finds it by name.
#
# Needs: scripts/fetch_blender_libs.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

LLVM="$BLENDER_LIB_DIR/llvm"
OUT="$ANARI_INSTALL/llvm-c/lib"
mkdir -p "$OUT"

libs="$("$LLVM/bin/llvm-config" --link-static --libfiles)"
system_libs="$("$LLVM/bin/llvm-config" --link-static --system-libs)"
echo "LLVM $("$LLVM/bin/llvm-config" --version): $(echo "$libs" | wc -w | tr -d ' ') static libraries"

# shellcheck disable=SC2086
log_run llvm-c-link xcrun clang++ -dynamiclib -arch arm64 -mmacosx-version-min=13.3 \
  -o "$OUT/libLLVM.dylib" \
  -install_name @rpath/libLLVM.dylib \
  -Wl,-all_load $libs $system_libs -lz -lxml2 -ledit \
  -Wl,-exported_symbol,'_LLVM*'
ls -l "$OUT/libLLVM.dylib"
echo "exported symbols: $(nm -gU "$OUT/libLLVM.dylib" | wc -l | tr -d ' '), of them not LLVM C API:" \
  "$(nm -gU "$OUT/libLLVM.dylib" | grep -v ' _LLVM' | wc -l | tr -d ' ')"
