#!/usr/bin/env bash
# build_moonray_deps.sh [name ...] - the third party libraries of MoonRay which Blender
# does not have, built into install/moonray-deps (sources and archives in
# external/moonray-deps, build trees in build/moonray-deps):
#
#   boost      1.86.0   filesystem, program_options, regex, thread (+ their dependencies)
#   jsoncpp    1.9.5
#   log4cplus  2.0.8
#   cppunit    1.15.1   only because scene_rdl2 asks for it; the tests are not built
#   random123  1.14.0   headers
#   ispc       1.20.0   the binary release, to tools/ (the compiler of MoonRay's ISPC code)
#   embree_isph 4.4.1  the ISPC headers of Embree (external/moonray-deps/embree-4.4.1)
#
# Everything else MoonRay needs comes from Blender's precompiled libraries
# (blenderphi/lib/linux_x64: TBB, Embree, OpenVDB, OpenEXR, Imath, OpenImageIO,
# OpenColorIO, OpenSubdiv, Open Image Denoise, zlib, jpeg), so that MoonRay and Blender
# use the same copy when the device is loaded into Blender, and from the system (Lua
# 5.4 with luac, OpenSSL, flex, bison, libatomic), see scripts/build_moonray.sh.
#
# None of the libraries built here has a counterpart in Blender, so they are shared
# libraries under their own names. Without arguments all of them are built; what is
# already installed is skipped.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

PREFIX="$ANARI_INSTALL/moonray-deps"
SRC="$ANARI_EXTERNAL/moonray-deps"
B="$ANARI_BUILD/moonray-deps"
mkdir -p "$PREFIX"/{bin,lib,include} "$SRC" "$B"

download() {   # download <url> <file>
  [ -s "$2" ] && return 0
  echo "downloading $1"
  curl -sS -L --fail -o "$2.part" "$1"
  mv "$2.part" "$2"
}

unpack() {   # unpack <archive> <directory which it creates>
  [ -d "$SRC/$2" ] && return 0
  tar -xf "$SRC/$1" -C "$SRC"
}

build_boost() {
  local v=1.86.0 d=boost-1.86.0
  [ -f "$PREFIX/lib/libboost_filesystem.so" ] && return 0
  download "https://github.com/boostorg/boost/releases/download/boost-$v/boost-$v-b2-nodocs.tar.xz" "$SRC/$d.tar.xz"
  unpack "$d.tar.xz" "$d"
  (
    cd "$SRC/$d"
    ./bootstrap.sh --prefix="$PREFIX" \
      --with-libraries=filesystem,program_options,regex,thread,system,atomic,chrono,date_time
    # --ignore-site-config / HOME: b2 would read user-config.jam from the home directory.
    HOME="$ANARI_CACHE/home" ./b2 -j"$JOBS" --ignore-site-config --build-dir="$B/boost" \
      variant=release link=shared threading=multi runtime-link=shared \
      --layout=system install
  )
}

build_jsoncpp() {
  local v=1.9.5 d=jsoncpp-1.9.5
  [ -f "$PREFIX/lib/libjsoncpp.so" ] && return 0
  download "https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/$v.tar.gz" "$SRC/$d.tar.gz"
  unpack "$d.tar.gz" "$d"
  cmake -S "$SRC/$d" -B "$B/jsoncpp" "${ANARI_CMAKE_COMMON[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF -DBUILD_OBJECT_LIBS=OFF \
    -DJSONCPP_WITH_TESTS=OFF -DJSONCPP_WITH_POST_BUILD_UNITTEST=OFF \
    -DJSONCPP_WITH_PKGCONFIG_SUPPORT=OFF
  cmake --build "$B/jsoncpp" --parallel "$JOBS"
  cmake --install "$B/jsoncpp"
}

build_log4cplus() {
  local v=2.0.8 d=log4cplus-2.0.8
  [ -f "$PREFIX/lib/liblog4cplus.so" ] && return 0
  download "https://github.com/log4cplus/log4cplus/releases/download/REL_${v//./_}/$d.tar.xz" "$SRC/$d.tar.xz"
  unpack "$d.tar.xz" "$d"
  cmake -S "$SRC/$d" -B "$B/log4cplus" "${ANARI_CMAKE_COMMON[@]}" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_SHARED_LIBS=ON -DUNICODE=OFF \
    -DLOG4CPLUS_BUILD_TESTING=OFF -DWITH_UNIT_TESTS=OFF -DLOG4CPLUS_BUILD_LOGGINGSERVER=OFF
  cmake --build "$B/log4cplus" --parallel "$JOBS"
  cmake --install "$B/log4cplus"
}

build_cppunit() {
  local d=cppunit-1.15.1
  [ -f "$PREFIX/lib/libcppunit.so" ] && return 0
  download "https://dev-www.libreoffice.org/src/$d.tar.gz" "$SRC/$d.tar.gz"
  unpack "$d.tar.gz" "$d"
  mkdir -p "$B/cppunit"
  (
    cd "$B/cppunit"
    "$SRC/$d/configure" --prefix="$PREFIX" --libdir="$PREFIX/lib" --disable-static --disable-doxygen
    make -j"$JOBS"
    make install
  )
}

build_random123() {
  local v=1.14.0 d=random123-1.14.0
  [ -f "$PREFIX/include/Random123/philox.h" ] && return 0
  download "https://github.com/DEShawResearch/random123/archive/refs/tags/v$v.tar.gz" "$SRC/$d.tar.gz"
  unpack "$d.tar.gz" "$d"
  cp -r "$SRC/$d/include/Random123" "$PREFIX/include/"
}

# The ISPC headers of Embree. MoonRay's ISPC code includes <embree4/rtcore.isph>;
# Blender's Embree is built without ISPC support and does not install them. They only
# declare the C API of the library, so the ones of the same version (4.4.1) fit.
build_embree_isph() {
  local v=4.4.1 d=embree-4.4.1
  [ -f "$SRC/$d/include/embree4/rtcore.isph" ] && return 0
  download "https://github.com/RenderKit/embree/archive/refs/tags/v$v.tar.gz" "$SRC/$d.tar.gz"
  tar -xzf "$SRC/$d.tar.gz" -C "$SRC" --wildcards "$d/include/embree4/*.isph"
}

build_ispc() {
  local v=1.20.0 d=ispc-v1.20.0-linux
  if [ ! -x "$ANARI_TOOLS/$d/bin/ispc" ]; then
    mkdir -p "$ANARI_TOOLS/dl"
    download "https://github.com/ispc/ispc/releases/download/v$v/$d.tar.gz" "$ANARI_TOOLS/dl/$d.tar.gz"
    tar -xzf "$ANARI_TOOLS/dl/$d.tar.gz" -C "$ANARI_TOOLS"
  fi
  ln -sfn "$ANARI_TOOLS/$d/bin/ispc" "$ANARI_TOOLS/bin/ispc"
}

names=("$@")
[ "${#names[@]}" = 0 ] && names=(ispc embree_isph random123 jsoncpp log4cplus cppunit boost)
for name in "${names[@]}"; do
  log_run "moonray-deps-$name" "build_$name"
done
ls "$PREFIX/lib" | grep -E '\.so$' | tr '\n' ' '
echo
echo "ispc: $("$ANARI_TOOLS/bin/ispc" --version)"
