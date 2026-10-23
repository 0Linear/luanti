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

target_link_libraries(${PROJECT_NAME} -Wl,--start-group ${VITA_SYSTEM_LIBS} -Wl,--end-group)
target_compile_definitions(${PROJECT_NAME} PRIVATE VITA_NEWLIB_HEAP_MB=${VITA_NEWLIB_HEAP_MB})

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
