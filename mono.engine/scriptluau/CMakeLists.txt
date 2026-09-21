# L10. The Luau half of the two-VM surface.
#
# Above `script` at L9, because everything a Luau script can name - the class
# tree, the property surface, the services, the signals, the tick - is described
# there with no VM in it. What lives here is the part that meets a `lua_State *`.
#
# `shared`, for `script`'s reason: a server-tier host runs game scripts and so
# does a client-tier one.
#
# **The vendor stops here, and that is the whole point of the module.** Until
# v0.19 Luau and QuickJS were both `VENDOR` on `script`, and the rule that kept
# the three kinds of file apart was a *filename* convention enforced by a
# sixty-line CMake closure walk. A file that meets the VM now lives in a module
# that links it; one that does not lives in a module that cannot. The layer check
# says the rest.
#
# **Beside `scriptjs` rather than above or below it.** Neither adapter may name
# the other: two languages over one binding surface means neither is the real
# one, and a module that hosted the other would decide that by accident.
# `scripthost` at L11 is where the choice lives.
mono_add_library(scriptluau
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::gui
		Engine::physics
		Engine::scene
		Engine::script
		Engine::world
	VENDOR
		# Package submission checks source before it can reach a scratch world.
		# This is not the normal game-script path, which remains compiler and VM
		# only.
		Vendor::luau_analysis
		Vendor::luau_compiler
		Vendor::luau_vm
)

# The package checker reads the generated binding contract rather than keeping a
# second handwritten type vocabulary. Configure also watches the source file, so
# a binding change rebuilds the embedded declaration on the next build.
set(scriptluau_declarations "${CMAKE_SOURCE_DIR}/mono.engine/script/bindings/engine.d.luau")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${scriptluau_declarations}")
file(READ "${scriptluau_declarations}" scriptluau_declaration_contents)

# MSVC refuses a raw string literal over 65,535 bytes. The declaration is already
# larger than that, so emit adjacent 16 KiB literals that C++ concatenates.
set(scriptluau_chunk_bytes 16384)
string(LENGTH "${scriptluau_declaration_contents}" scriptluau_total)
set(SCRIPT_LUAU_DECLARATION_LITERALS "")
set(scriptluau_index 0)
set(scriptluau_offset 0)
while(scriptluau_offset LESS scriptluau_total)
	string(SUBSTRING "${scriptluau_declaration_contents}" ${scriptluau_offset} ${scriptluau_chunk_bytes}
		scriptluau_chunk)
	string(APPEND SCRIPT_LUAU_DECLARATION_LITERALS
		"\tR\"ENGINE_LUAU_${scriptluau_index}(${scriptluau_chunk})ENGINE_LUAU_${scriptluau_index}\"\n")
	math(EXPR scriptluau_offset "${scriptluau_offset} + ${scriptluau_chunk_bytes}")
	math(EXPR scriptluau_index "${scriptluau_index} + 1")
endwhile()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageLuauDeclarations.hpp.in"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/PackageLuauDeclarations.hpp"
	@ONLY)
target_include_directories(engine_scriptluau PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
