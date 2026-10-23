#!/bin/bash -e
# Builds luanti.vpk for the PlayStation Vita.
#
# Requirements:
#  - VitaSDK with the vdpm packages (the vitasdk/vitasdk Docker image has
#    everything): sdl2 (vitaGL backend), vitaGL, vitaShaRK, luajit, curl,
#    openal-soft, libvorbis, freetype, libpng, libjpeg-turbo, libzip, zstd,
#    libintl
#  - cmake, ninja (or make), git, curl, unzip; msgfmt for translations
#
# Usage: util/vita/build.sh [build directory]
#
# Environment:
#  VITASDK            VitaSDK location (default: /usr/local/vitasdk)
#  STOCK_VITAGL=1     link the vdpm vitaGL instead of building one with the
#                     shader cache (slow startup: every shader is recompiled
#                     on every launch)
#  ENABLE_LTO=0       disable link time optimization (faster builds)
#  CMAKE_BUILD_TYPE   default: Release

topdir=$(cd "$(dirname "$0")/../.." && pwd)
builddir=$(realpath -m "${1:-$topdir/build/vita}")
export VITASDK=${VITASDK:-/usr/local/vitasdk}
export PATH="$VITASDK/bin:$PATH"

[ -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || {
	echo "VitaSDK not found at $VITASDK (set VITASDK)" >&2
	exit 1
}

mkdir -p "$builddir/deps"
cd "$builddir/deps"

## SQLite amalgamation (there is no vdpm package)
sqlite_ver=3500400
sqlite_year=2025
if [ ! -f sqlite/sqlite3.c ]; then
	curl -fL -o sqlite.zip \
		"https://www.sqlite.org/$sqlite_year/sqlite-amalgamation-$sqlite_ver.zip"
	unzip -q -o sqlite.zip
	rm -rf sqlite
	mv "sqlite-amalgamation-$sqlite_ver" sqlite
	rm sqlite.zip
fi

## vitaGL with the shader cache enabled
# Same commit as vdpm, built with:
#  HAVE_SHADER_CACHE=1  compiled shaders are kept on the memory card
#  DRAW_SPEEDHACK=2     cheaper draw call setup for VBO based draws
#  NO_DEBUG=1           no GL error checking
vitagl_commit=2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8
vitagl_args=()
if [ "${STOCK_VITAGL:-0}" != 1 ]; then
	if [ ! -f vitaGL/libvitaGL.a ]; then
		[ -d vitaGL ] || git clone https://github.com/Rinnegatamante/vitaGL vitaGL
		git -C vitaGL checkout -q "$vitagl_commit"
		make -C vitaGL -j"$(nproc)" \
			NO_DEBUG=1 HAVE_SHADER_CACHE=1 DRAW_SPEEDHACK=2
	fi
	vitagl_args=(-DVITA_VITAGL_DIR="$builddir/deps/vitaGL")
fi

cd "$builddir"
generator=()
command -v ninja >/dev/null && generator=(-G Ninja)

cmake "$topdir" "${generator[@]}" \
	-DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
	-DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
	-DENABLE_LTO="${ENABLE_LTO:-1}" \
	-DSQLITE3_AMALGAMATION_DIR="$builddir/deps/sqlite" \
	"${vitagl_args[@]}"

cmake --build . --parallel "$(nproc)"

echo
echo "Done: $builddir/luanti.vpk"
