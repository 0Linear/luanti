# PlayStation Vita: link the client against the system libraries and turn it
# into an installable VPK.
#
# The VPK is assembled by running the regular install() rules into a staging
# directory (so it ships exactly what a desktop RUN_IN_PLACE install would:
# builtin, client shaders, textures, fonts, locales, ...), then adding the
# eboot.bin and the LiveArea assets, and zipping the result.

set(VITA_TITLEID "LUAN00001" CACHE STRING "Vita title ID (9 characters)")
set(VITA_APP_NAME "Luanti" CACHE STRING "Name shown in the LiveArea")
set(VITA_NEWLIB_HEAP_MB 224 CACHE STRING
	"Size of the malloc heap in MiB. The rest of the memory budget goes to vitaGL.")

# APP_VER must be ##.##
math(EXPR _vita_minor "${VERSION_MINOR}")
if(_vita_minor LESS 10)
	set(_vita_minor "0${_vita_minor}")
endif()
set(VITA_APP_VER "0${VERSION_MAJOR}.${_vita_minor}")

# vdpm's vitaGL is built without the shader cache; util/vita/build.sh builds
# one with it (and some speedhacks). Its directory goes first in the search path.
set(VITA_VITAGL_DIR "" CACHE PATH "Directory containing a custom libvitaGL.a")
if(VITA_VITAGL_DIR)
	if(NOT EXISTS "${VITA_VITAGL_DIR}/libvitaGL.a")
		message(FATAL_ERROR "No libvitaGL.a in VITA_VITAGL_DIR=${VITA_VITAGL_DIR}")
	endif()
	target_link_directories(${PROJECT_NAME} BEFORE PRIVATE "${VITA_VITAGL_DIR}")
	message(STATUS "Using vitaGL from ${VITA_VITAGL_DIR}")
endif()

target_link_libraries(${PROJECT_NAME} -Wl,--start-group ${VITA_SYSTEM_LIBS} -Wl,--end-group)

# vita-elf-create appends its import/export tables right after the code
# segment and fails ("segment 1 overlaps") when the data segment starts
# less than a few KiB after it, which depends on the code size. Link with
# the default script, changed to always leave at least 16 KiB in between.
execute_process(COMMAND ${CMAKE_LINKER} --verbose OUTPUT_VARIABLE VITA_LD_VERBOSE)
string(FIND "${VITA_LD_VERBOSE}" "==================================================" VITA_LD_START)
string(FIND "${VITA_LD_VERBOSE}" "==================================================" VITA_LD_END REVERSE)
if(VITA_LD_START EQUAL -1 OR VITA_LD_END LESS_EQUAL VITA_LD_START)
	message(FATAL_ERROR "Could not get the default linker script from ${CMAKE_LINKER}")
endif()
math(EXPR VITA_LD_START "${VITA_LD_START} + 50")
math(EXPR VITA_LD_LENGTH "${VITA_LD_END} - ${VITA_LD_START}")
string(SUBSTRING "${VITA_LD_VERBOSE}" ${VITA_LD_START} ${VITA_LD_LENGTH} VITA_LD_SCRIPT)
string(REGEX REPLACE "\\. = DATA_SEGMENT_ALIGN \\([^)]*\\);"
	". = ALIGN(. + 0x4000, 0x1000);" VITA_LD_SCRIPT_GAP "${VITA_LD_SCRIPT}")
# These only make sense together with DATA_SEGMENT_ALIGN
string(REGEX REPLACE "\\. = DATA_SEGMENT_RELRO_END \\([^)]*\\);" ""
	VITA_LD_SCRIPT_GAP "${VITA_LD_SCRIPT_GAP}")
string(REGEX REPLACE "\\. = DATA_SEGMENT_END \\([^)]*\\);" ""
	VITA_LD_SCRIPT_GAP "${VITA_LD_SCRIPT_GAP}")
if(VITA_LD_SCRIPT_GAP STREQUAL VITA_LD_SCRIPT)
	message(FATAL_ERROR "Unexpected default linker script: no DATA_SEGMENT_ALIGN")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/vita_gap.ld" "${VITA_LD_SCRIPT_GAP}")
target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-T,${CMAKE_BINARY_DIR}/vita_gap.ld")
target_compile_definitions(${PROJECT_NAME} PRIVATE VITA_NEWLIB_HEAP_MB=${VITA_NEWLIB_HEAP_MB})

# The Cortex-A9 NEON unit flushes denormals to zero, so GCC only
# auto-vectorizes float loops with -funsafe-math-optimizations. Allow that for
# the number crunching that doesn't depend on exact IEEE semantics: noise and
# map generation, lighting and mesh generation. (Not globally: other code
# relies on NaN checks and exact comparisons.)
file(GLOB _vita_fast_math_srcs
	"${CMAKE_CURRENT_SOURCE_DIR}/mapgen/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/client/meshgen/*.cpp")
list(APPEND _vita_fast_math_srcs
	"${CMAKE_CURRENT_SOURCE_DIR}/noise.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/voxelalgorithms.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/client/content_mapblock.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/client/mapblock_mesh.cpp")
set_source_files_properties(${_vita_fast_math_srcs} PROPERTIES
	COMPILE_OPTIONS "-funsafe-math-optimizations")

include("${VITASDK}/share/vita.cmake" REQUIRED)

# UNSAFE: LuaJIT allocates executable memory for its JIT compiler
# (sceKernelAllocMemBlockForVM), which needs the unsafe homebrew flag.
vita_create_self(eboot.bin "${EXECUTABLE_OUTPUT_PATH}/${PROJECT_NAME}" UNSAFE)
add_dependencies(eboot.bin-self ${PROJECT_NAME})
add_dependencies(${PROJECT_NAME}-velf ${PROJECT_NAME})

# ATTRIBUTE2=12 requests the extended memory budget (+109 MiB).
set(_vita_sfo "${CMAKE_CURRENT_BINARY_DIR}/vita_param.sfo")
add_custom_command(OUTPUT "${_vita_sfo}"
	COMMAND ${VITA_MKSFOEX}
		-s APP_VER=${VITA_APP_VER}
		-s TITLE_ID=${VITA_TITLEID}
		-d ATTRIBUTE2=12
		"${VITA_APP_NAME}" "${_vita_sfo}"
	COMMENT "Generating param.sfo"
	VERBATIM)

set(_vita_stage "${CMAKE_BINARY_DIR}/vpk_stage")
set(_vita_vpk "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.vpk")
set(_vita_sce_sys "${CMAKE_SOURCE_DIR}/misc/vita/sce_sys")

add_custom_command(OUTPUT "${_vita_vpk}"
	COMMAND ${CMAKE_COMMAND} -E rm -rf "${_vita_stage}"
	COMMAND ${CMAKE_COMMAND} --install "${CMAKE_BINARY_DIR}"
		--prefix "${_vita_stage}" --component Unspecified
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${_vita_sce_sys}" "${_vita_stage}/sce_sys"
	COMMAND ${CMAKE_COMMAND} -E copy "${_vita_sfo}" "${_vita_stage}/sce_sys/param.sfo"
	COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" "${_vita_stage}/eboot.bin"
	COMMAND ${CMAKE_COMMAND} -E rm -f "${_vita_vpk}"
	COMMAND ${CMAKE_COMMAND} -E chdir "${_vita_stage}"
		${CMAKE_COMMAND} -E tar cf "${_vita_vpk}" --format=zip .
	DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin" "${_vita_sfo}" eboot.bin-self
	COMMENT "Packing ${PROJECT_NAME}.vpk"
	VERBATIM)

add_custom_target(vpk ALL DEPENDS "${_vita_vpk}")
if(TARGET translations)
	add_dependencies(vpk translations)
endif()
