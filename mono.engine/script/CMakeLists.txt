# L9. Running a script against a world.
#
# Above `scene` at L7, because a script's whole vocabulary is the class tree and
# the property surface that module declares — `Instance.new("Part")` resolves
# through `ecs::Classes` and lands in `scene`'s components. Below anything that
# presents: a script builds a world and does not draw one.
#
# `shared`, and it has to be. A server-tier host runs game scripts and so does a
# client-tier one; a scripting layer that could only exist on one side would put
# the game's own logic on the wrong side of the tier check.
#
# **The VM is `VENDOR` rather than `VENDOR_PUBLIC`, and that is the invariant
# worth keeping.** No `lua_State` appears in this module's public headers — the
# one in `Runtime.hpp` is a forward declaration and a pointer. So no consumer
# compiles against Luau, nobody outside can call the VM directly, and swapping
# or adding a second VM is a change inside this directory. `v05.md` §5.7 has
# JavaScript arriving beside Luau rather than instead of it; this is what makes
# that a second source file rather than a second module.
#
# Does not depend on `world` or `physics`. A script reaches a world through the
# `Store` its host hands it, which is the same seam every other module uses, and
# nothing here should learn what a tick is before v0.6 gives scripts something
# to yield on.
mono_add_library(script
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::scene

		# L7, `shared`. **The same edge as `scene`, one dimension down.** A
		# script's vocabulary is the class tree, and there are two of them: a
		# `Part` comes from `scene` and a `TextButton` from `gui`. Depending on
		# one and not the other would make `.Activated` reachable only through
		# something above this module — and that something would be a second
		# path into a VM, which is exactly what `gui/Input.hpp` refuses to
		# invent when it says the join belongs to whoever owns the scripting
		# layer.
		#
		# What crosses is `gui::GuiEvent`: a kind, an entity and two points.
		# Nothing here draws, resolves a texture or knows what a canvas is.
		Engine::gui

		# L4, `shared`. **The Universe's services sit on these buses.** A script
		# reaches another world through `MessagingService` and through nothing
		# else — `world::Postbox` is the crossing, and rule 3 is why there is no
		# other one: nothing crossing a world boundary is a pointer, so a second
		# route would have to be a second copy of the same message.
		#
		# A thin view over two resources rather than a subsystem, so this buys
		# the seam and no machinery.
		Engine::world

		# L8. **`WorldRoot:Raycast` is why.** `RaycastParams` and
		# `RaycastResult` are the one part of the datatype vocabulary that is not
		# a value type — they are a *query*, and the query that answers them is
		# `physics::Raycast` against the exact shapes rather than
		# `spatial::Raycast` against the proxy boxes. A ray that reported the
		# bounding box of a rotated part would be plausible and wrong, which is
		# the failure `physics/Query.hpp` says that function exists for.
		#
		# The only edge from `script` to a simulation module, and it is one-way:
		# nothing in `physics` knows a script exists.
		Engine::physics
	VENDOR
		Vendor::luau_vm
		Vendor::luau_compiler
		Vendor::quickjs
)

# Every file in `src/` that meets a VM is named for that VM.
#
# **The three kinds of file in this directory, checked rather than written
# down.** A service here is now three things — internal machinery that runs per
# tick whether or not a script exists, a `ServiceSurface` describing what a
# script may call, and the per-VM adapter that meets a `lua_State *` or a
# `JSContext *` — and the split is only worth having if the build refuses to let
# it rot. Rule 6: a rule the build does not check is documentation.
#
# The rule is one sentence. **A translation unit or private header that reaches
# `<lua.h>` or `<quickjs.h>`, however many hops away, has a name beginning
# `Luau`, `Js` or `JavaScript`.** Everything else compiles in a build that never
# opens a VM, which is what makes `AdmitTeleports` a system rather than a
# binding and `UserInputService.cpp` a description rather than a Luau file.
#
# Transitive, because the coupling that matters arrives through an umbrella
# rather than through `#include <lua.h>`. `LuauBindings.hpp` was called
# `Bindings.hpp` until v0.18 and eleven neutral files included it for one
# constant apiece; nothing said so, because none of them named Luau.
#
# Re-read on every configure, and `just build` configures every time — so this
# fires on the commit that introduces the edge rather than on the next time
# somebody adds a file.
function(mono_check_script_vm_naming)
	file(GLOB _vm_sources CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")

	# `ServiceCatalogue.cpp` is the one file allowed to have met both VMs, and
	# its own header comment carries why: it names every installer so the static
	# archive cannot drop one, and it walks the table once per language. A second
	# name on this list is a claim that a file cannot be split, and it needs the
	# same kind of argument written where the file is.
	set(_vm_exempt "ServiceCatalogue.cpp")

	set(_vm_headers "")
	foreach(_source IN LISTS _vm_sources)
		file(READ "${_source}" _text)
		if(_text MATCHES "#include[ \t]*[<\"](lua|lualib|luaconf|luacode|quickjs)")
			get_filename_component(_name "${_source}" NAME)
			list(APPEND _vm_headers "${_name}")
		endif()
	endforeach()

	# Closure: a file including a VM-touching header is VM-touching itself.
	# Bounded by the file count, and the list only ever grows.
	set(_growing TRUE)
	while(_growing)
		set(_growing FALSE)
		foreach(_source IN LISTS _vm_sources)
			get_filename_component(_name "${_source}" NAME)
			if(_name IN_LIST _vm_headers)
				continue()
			endif()
			file(READ "${_source}" _text)
			foreach(_header IN LISTS _vm_headers)
				if(_text MATCHES "#include[ \t]*\"${_header}\"")
					list(APPEND _vm_headers "${_name}")
					set(_growing TRUE)
					break()
				endif()
			endforeach()
		endforeach()
	endwhile()

	set(_offenders "")
	foreach(_name IN LISTS _vm_headers)
		if(_name IN_LIST _vm_exempt)
			continue()
		endif()
		if(NOT _name MATCHES "^(Luau|Js|JavaScript)")
			list(APPEND _offenders "${_name}")
		endif()
	endforeach()

	if(_offenders)
		list(JOIN _offenders "\n    " _report)
		message(FATAL_ERROR
			"mono.engine/script: these reach a scripting VM but are not named for one:\n"
			"    ${_report}\n"
			"A file that meets a lua_State or a JSContext is an adapter and its name says "
			"which VM. Everything else in src/ compiles with no VM present. Either rename "
			"it Luau*/Js*, or split the part that needs the VM out into an adapter of that "
			"name — see mono.engine/script/AGENTS.md.")
	endif()
endfunction()

mono_check_script_vm_naming()
