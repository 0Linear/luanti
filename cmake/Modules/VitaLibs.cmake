# PlayStation Vita (VitaSDK) build defaults and dependency locations.
#
# Included from the top-level CMakeLists.txt when the VitaSDK toolchain file
# (which sets VITA=TRUE) is in use. Every library comes from the VitaSDK
# sysroot (installed with `vdpm`), except SQLite which has no vdpm package and
# is built from the amalgamation (see SQLITE3_AMALGAMATION_DIR).

if(NOT DEFINED ENV{VITASDK} AND NOT VITASDK)
	message(FATAL_ERROR "VITASDK is not set; use -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake")
endif()

set(VITA_SYSROOT "${VITASDK}/arm-vita-eabi")
set(VITA_LIBDIR "${VITA_SYSROOT}/lib")
set(VITA_INCDIR "${VITA_SYSROOT}/include")

# Only the client makes sense on the Vita. Singleplayer still runs an
# in-process server, which is part of the client binary.
set(BUILD_SERVER FALSE CACHE BOOL "Build server" FORCE)
# Unit tests and benchmarks can be enabled to run them in Vita3K
set(BUILD_UNITTESTS FALSE CACHE BOOL "Build unittests")
set(BUILD_BENCHMARKS FALSE CACHE BOOL "Build benchmarks")
set(BUILD_DOCUMENTATION FALSE CACHE BOOL "Build documentation" FORCE)
set(RUN_IN_PLACE TRUE CACHE BOOL "Run directly in source directory structure" FORCE)

# Database/backends that do not exist on the Vita
set(ENABLE_POSTGRESQL FALSE CACHE BOOL "" FORCE)
set(ENABLE_LEVELDB FALSE CACHE BOOL "" FORCE)
set(ENABLE_REDIS FALSE CACHE BOOL "" FORCE)
set(ENABLE_SPATIAL FALSE CACHE BOOL "" FORCE)
set(ENABLE_CURSES FALSE CACHE BOOL "" FORCE)
set(ENABLE_PROMETHEUS FALSE CACHE BOOL "" FORCE)
# vdpm ships OpenSSL 1.1, Luanti wants 3.x for its SHA fast path
set(ENABLE_OPENSSL FALSE CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_GMP FALSE CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_JSONCPP FALSE CACHE BOOL "" FORCE)

# Rendering: vitaGL exposes OpenGL ES 2.0 + GLSL ES 1.00 (translated to Cg)
set(ENABLE_OPENGL FALSE CACHE BOOL "" FORCE)
set(ENABLE_OPENGL3 FALSE CACHE BOOL "" FORCE)
set(ENABLE_GLES2 TRUE CACHE BOOL "" FORCE)
set(USE_SDL2_STATIC TRUE CACHE BOOL "" FORCE)
set(USE_SDL3 FALSE CACHE BOOL "" FORCE)

set(ENABLE_LUAJIT TRUE CACHE BOOL "" FORCE)
set(REQUIRE_LUAJIT TRUE CACHE BOOL "" FORCE)

# Explicit library locations (avoids picking up host libraries by accident).
# These need to be cache entries since the find modules use find_library().
# Values given on the command line (e.g. a self-built cURL) take precedence.
macro(vita_dep var value)
	if(NOT ${var} OR "${${var}}" MATCHES "-NOTFOUND$")
		set(${var} "${value}" CACHE STRING "" FORCE)
	endif()
endmacro()
vita_dep(ZLIB_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(ZLIB_LIBRARY "${VITA_LIBDIR}/libz.a")
vita_dep(ZSTD_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(ZSTD_LIBRARY "${VITA_LIBDIR}/libzstd.a;-pthread")
vita_dep(PNG_PNG_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(PNG_LIBRARY "${VITA_LIBDIR}/libpng16.a")
vita_dep(JPEG_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(JPEG_LIBRARY "${VITA_LIBDIR}/libjpeg.a")
vita_dep(FREETYPE_INCLUDE_DIR_ft2build "${VITA_INCDIR}/freetype2")
vita_dep(FREETYPE_INCLUDE_DIR_freetype2 "${VITA_INCDIR}/freetype2/freetype")
vita_dep(FREETYPE_LIBRARY "${VITA_LIBDIR}/libfreetype.a;${VITA_LIBDIR}/libbz2.a;${VITA_LIBDIR}/libpng16.a;${VITA_LIBDIR}/libz.a")
vita_dep(LIBZIP_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(LIBZIP_LIBRARY "${VITA_LIBDIR}/libzip.a")
vita_dep(LUA_INCLUDE_DIR "${VITA_INCDIR}/luajit-2.1")
vita_dep(LUA_LIBRARY "${VITA_LIBDIR}/libluajit-5.1.a;${VITA_LIBDIR}/libdl.a;-lSceSblSsMgr_stub;-ltaihen_stub;-lSceKernelModulemgr_stub;-lSceSysmem_stub;-lSceLibKernel_stub")
vita_dep(OGG_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(OGG_LIBRARY "${VITA_LIBDIR}/libogg.a")
vita_dep(VORBIS_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(VORBIS_LIBRARY "${VITA_LIBDIR}/libvorbis.a")
vita_dep(VORBISFILE_LIBRARY "${VITA_LIBDIR}/libvorbisfile.a")
vita_dep(OPENAL_INCLUDE_DIR "${VITA_INCDIR}/AL")
vita_dep(OPENAL_LIBRARY "${VITA_LIBDIR}/libopenal.a")
vita_dep(CURL_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(CURL_LIBRARY "${VITA_LIBDIR}/libcurl.a;${VITA_LIBDIR}/libssl.a;${VITA_LIBDIR}/libcrypto.a")
vita_dep(GETTEXT_INCLUDE_DIR "${VITA_INCDIR}")
vita_dep(GETTEXT_LIBRARY "${VITA_LIBDIR}/libintl.a")

# SQLite: no vdpm package exists, so compile the amalgamation ourselves with
# options tuned for a single-user, memory-constrained console.
set(SQLITE3_AMALGAMATION_DIR "" CACHE PATH
	"Directory containing sqlite3.c and sqlite3.h (SQLite amalgamation)")
if(NOT SQLITE3_AMALGAMATION_DIR OR NOT EXISTS "${SQLITE3_AMALGAMATION_DIR}/sqlite3.c")
	message(FATAL_ERROR "Vita build needs the SQLite amalgamation. Download it from "
		"https://www.sqlite.org/download.html and pass -DSQLITE3_AMALGAMATION_DIR=<dir> "
		"(util/vita/build.sh does this for you).")
endif()
add_library(vita_sqlite3 STATIC
	"${SQLITE3_AMALGAMATION_DIR}/sqlite3.c"
	"${CMAKE_SOURCE_DIR}/lib/sqlite_vita/sqlite3_vfs_vita.c")
target_include_directories(vita_sqlite3 PUBLIC "${SQLITE3_AMALGAMATION_DIR}")
target_compile_options(vita_sqlite3 PRIVATE -O2 -w)
target_compile_definitions(vita_sqlite3 PRIVATE
	SQLITE_OS_OTHER=1             # our own VFS: lib/sqlite_vita
	SQLITE_OMIT_WAL=1             # WAL needs shared memory
	SQLITE_MAX_MMAP_SIZE=0
	SQLITE_OMIT_LOAD_EXTENSION=1
	SQLITE_OMIT_DEPRECATED=1
	SQLITE_OMIT_SHARED_CACHE=1
	SQLITE_OMIT_PROGRESS_CALLBACK=1
	SQLITE_OMIT_AUTHORIZATION=1
	SQLITE_DEFAULT_MEMSTATUS=0    # avoids a global mutex on every malloc
	SQLITE_DEFAULT_SYNCHRONOUS=1
	SQLITE_DEFAULT_PAGE_SIZE=4096
	SQLITE_DEFAULT_CACHE_SIZE=-2048 # 2 MiB page cache per connection
	SQLITE_TEMP_STORE=3           # temp tables in RAM, not on the memory card
	SQLITE_THREADSAFE=2           # each connection is used by one thread at a time
	SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
)
vita_dep(SQLITE3_INCLUDE_DIR "${SQLITE3_AMALGAMATION_DIR}")
vita_dep(SQLITE3_LIBRARY vita_sqlite3)

# System module stubs needed by our own Vita code (src/porting_vita.cpp) and
# by the static libraries above. SDL2's own stubs come from its CMake config.
set(VITA_SYSTEM_LIBS
	SceAppMgr_stub
	SceAppUtil_stub
	SceCommonDialog_stub
	SceCtrl_stub
	SceDisplay_stub
	SceGxm_stub
	SceIme_stub
	SceKernelDmacMgr_stub
	SceLibKernel_stub
	SceKernelThreadMgr_stub
	SceKernelModulemgr_stub
	SceIofilemgr_stub
	SceNet_stub
	SceNetCtl_stub
	SceHttp_stub
	SceSsl_stub
	ScePower_stub
	SceProcessmgr_stub
	SceRtc_stub
	SceShaccCg_stub
	SceShaccCgExt
	SceSysmem_stub
	SceSysmodule_stub
	SceTouch_stub
	SceMotion_stub
	SceHid_stub
	SceAudio_stub
	SceAudioIn_stub
	taihen_stub
	vitaGL
	vitashark
	mathneon
	# private dependencies of the static libzip
	lzma
	bz2
	m
)
