# The Luau type checker, run against the generated declarations.
#
# **The only first-party target that links `Luau.Analysis`,** and it is a tool
# rather than a module for that reason: nothing a game binary contains
# type-checks anything. A shipped runtime compiles bytecode and runs it, so the
# analysis half of Luau is a build-time dependency exactly like clang-format is.
#
# A bare `add_executable` with no library behind it, the same shape as
# `bindings`. There is no pure function in here to test — the tool is a driver
# over `Luau::Frontend`, and a suite over it would be a suite over Luau. What
# proves it works is that `just typecheck` fails when a script names something
# the bindings do not have, which is a thing the repository can demonstrate
# rather than assert.
#
# Worth knowing, as `bindings` records for itself: `expected_graph.json` does
# not carry this row. The graph check tracks `mono_add_library` modules and
# `mono_add_program` programs, and a bare tool is neither — so this link row is
# reviewed here and nowhere else.
add_executable(scriptcheck app/main.cpp)
target_link_libraries(scriptcheck PRIVATE
	# Arguments and Log, so `scriptcheck --help` looks like every other --help
	# in the repository.
	Engine::core

	# The type checker. Aliased in MonoVendor.cmake for this target and no
	# other; the comment there is the whole argument.
	Vendor::luau_analysis)
target_compile_options(scriptcheck PRIVATE ${MONO_COMPILE_OPTIONS})

set_target_properties(scriptcheck PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/tools")
