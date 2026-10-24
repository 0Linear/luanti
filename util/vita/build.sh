#!/bin/bash -e
# Builds luanti.vpk for the PlayStation Vita.
#
# Requirements:
#  - VitaSDK with the vdpm packages (the vitasdk/vitasdk Docker image has
#    everything): sdl2 (vitaGL backend), vitaGL, vitaShaRK, luajit, curl,
#    openal-soft, libvorbis, freetype, libpng, libjpeg-turbo, libzip, zstd,
#    libintl
#  - cmake, ninja, git, curl, unzip; msgfmt for translations
#
# Usage: util/vita/build.sh [build directory]
#
# Environment:
#  VITASDK            VitaSDK location (default: /usr/local/vitasdk)
#  STOCK_VITAGL=1     link the vdpm vitaGL instead of building one with the
#                     shader cache (slow startup: every shader is recompiled
#                     on every launch)
#  ENABLE_LTO=0       disable link time optimization (faster builds)
#  BUILD_CURL, BUILD_LIBZIP  1 = always build from source, 0 = never,
#                     auto (default) = only if the SDK's copy is broken
#  CMAKE_BUILD_TYPE   default: Release
#  VITA_CMAKE_ARGS    extra CMake arguments, e.g.
#                     "-DVITA_BUILD_TESTS=ON -DBUILD_UNITTESTS=ON" to run the
#                     unit tests in Vita3K (luanti --run-unittests)

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

## cURL and libzip
# Some VitaSDK installs (e.g. older Docker images) ship these built against
# OpenSSL 1.0 headers, which then fail to link against the OpenSSL 1.1
# libraries next to them. Build our own copies in that case.
sysroot="$VITASDK/arm-vita-eabi"
prefix="$builddir/deps/prefix"
dep_args=()
vita_cmake=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake"
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF)
stale_openssl() { arm-vita-eabi-nm -u "$1" 2>/dev/null | grep -qE " (sk_value|HMAC_CTX_init)\$"; }

if [ "${BUILD_CURL:-auto}" = 1 ] || { [ "${BUILD_CURL:-auto}" = auto ] && stale_openssl "$sysroot/lib/libcurl.a"; }; then
	if [ ! -f "$prefix/lib/libcurl.a" ]; then
		[ -d curl ] || git clone --depth 1 --branch curl-8_17_0 https://github.com/curl/curl curl
		cmake -S curl -B curl/build "${vita_cmake[@]}" \
			-DCMAKE_C_FLAGS=-DOPENSSL_NO_ENGINE -DCMAKE_REQUIRED_FLAGS=-pthread \
			-DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DENABLE_IPV6=OFF \
			-DCURL_DISABLE_SOCKETPAIR=ON -DHAVE_FCNTL_O_NONBLOCK=OFF \
			-DENABLE_THREADED_RESOLVER=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
			-DENABLE_CURL_MANUAL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
			-DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_USE_OPENSSL=ON \
			-DCURL_CA_BUNDLE="vs0:data/external/cert/CA_LIST.cer"
		sed -i '/HAVE_PIPE2/d' curl/build/lib/curl_config.h
		cmake --build curl/build && cmake --install curl/build
	fi
	dep_args+=(-DCURL_INCLUDE_DIR="$prefix/include"
		-DCURL_LIBRARY="$prefix/lib/libcurl.a;$sysroot/lib/libssl.a;$sysroot/lib/libcrypto.a")
fi

if [ "${BUILD_LIBZIP:-auto}" = 1 ] || { [ "${BUILD_LIBZIP:-auto}" = auto ] && stale_openssl "$sysroot/lib/libzip.a"; }; then
	if [ ! -f "$prefix/lib/libzip.a" ]; then
		[ -d libzip ] || git clone --depth 1 --branch v1.11.4 https://github.com/nih-at/libzip libzip
		# Luanti never reads encrypted archives: no crypto backend needed
		cmake -S libzip -B libzip/build "${vita_cmake[@]}" \
			-DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
			-DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DHAVE_ARC4RANDOM=OFF \
			-DENABLE_OPENSSL=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF \
			-DENABLE_COMMONCRYPTO=OFF -DENABLE_WINDOWS_CRYPTO=OFF
		cmake --build libzip/build && cmake --install libzip/build
	fi
	dep_args+=(-DLIBZIP_INCLUDE_DIR="$prefix/include" -DLIBZIP_LIBRARY="$prefix/lib/libzip.a")
fi

cd "$builddir"
cmake "$topdir" -G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
	-DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
	-DENABLE_LTO="${ENABLE_LTO:-1}" \
	-DSQLITE3_AMALGAMATION_DIR="$builddir/deps/sqlite" \
	"${vitagl_args[@]}" "${dep_args[@]}" ${VITA_CMAKE_ARGS:-}

cmake --build . --parallel "$(nproc)"

echo
echo "Done: $builddir/luanti.vpk"
