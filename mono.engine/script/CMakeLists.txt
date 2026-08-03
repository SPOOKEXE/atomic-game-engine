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
