# The Luau type checker over the engine declarations.
#
# A library so its suite can check fixture plugins and drive the language
# server in-process, and a thin program over it. Nothing a game binary contains
# type-checks anything, so this stays a tool: a shipped runtime compiles
# bytecode and runs it. Studio reaches it through the staged program (the
# control surface's `script_check`), not by linking `Luau.Analysis`.
mono_add_library(scriptcheck
	TIER shared
	NAMESPACE Tool
	DEPS Engine::core
	VENDOR Vendor::luau_analysis Vendor::json
)

add_executable(scriptcheck app/main.cpp)
# `Vendor::luau_codegen` only to ask whether this CPU has the native backend,
# which `--describe` reports as `pluginBackend`.
target_link_libraries(scriptcheck PRIVATE Tool::scriptcheck Engine::core Vendor::json Vendor::luau_codegen)
target_compile_options(scriptcheck PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(scriptcheck PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

# The declarations beside the program, so a copy of `tools/` used outside the
# checkout finds them without a flag. A file-level rule rather than POST_BUILD,
# so editing the bindings alone restages them without relinking scriptcheck.
set(_scriptcheck_defs_src "${PROJECT_SOURCE_DIR}/mono.engine/script/bindings/engine.d.luau")
set(_scriptcheck_defs_out "${MONO_STAGE_ROOT}/tools/engine.d.luau")
add_custom_command(OUTPUT "${_scriptcheck_defs_out}"
	COMMAND "${CMAKE_COMMAND}" -E copy "${_scriptcheck_defs_src}" "${_scriptcheck_defs_out}"
	DEPENDS "${_scriptcheck_defs_src}"
	VERBATIM)
add_custom_target(scriptcheck_definitions DEPENDS "${_scriptcheck_defs_out}")
add_dependencies(scriptcheck scriptcheck_definitions)

if(TARGET test_scriptcheck)
	target_link_libraries(test_scriptcheck PRIVATE Vendor::json)
endif()
