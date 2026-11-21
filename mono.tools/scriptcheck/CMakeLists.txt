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
target_link_libraries(scriptcheck PRIVATE Tool::scriptcheck Engine::core Vendor::json)
target_compile_options(scriptcheck PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(scriptcheck PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")

# The declarations beside the program, so a copy of `tools/` used outside the
# checkout finds them without a flag.
add_custom_command(TARGET scriptcheck POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"${PROJECT_SOURCE_DIR}/mono.engine/script/bindings/engine.d.luau"
		"$<TARGET_FILE_DIR:scriptcheck>/engine.d.luau"
	VERBATIM)

if(TARGET test_scriptcheck)
	target_link_libraries(test_scriptcheck PRIVATE Vendor::json)
endif()
