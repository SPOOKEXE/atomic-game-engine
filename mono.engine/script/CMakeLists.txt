# L9. Running a script against a world.
#
# Above `scene` at L7, because a script's whole vocabulary is the class tree and
# the property surface that module declares - `Instance.new("Part")` resolves
# through `ecs::Classes` and lands in `scene`'s components. Below anything that
# presents: a script builds a world and does not draw one.
#
# `shared`, and it has to be. A server-tier host runs game scripts and so does a
# client-tier one; a scripting layer that could only exist on one side would put
# the game's own logic on the wrong side of the tier check.
#
# **No vendor at all, and that is the invariant this module now carries.** Until
# v0.19 both VMs were `VENDOR` here and the boundary between "what a script may
# name" and "how a VM builds it" was a *filename* convention, checked by a
# sixty-line CMake closure walk in this file. It is a module boundary now:
# `scriptluau` and `scriptjs` sit at L10 above this, `scripthost` at L11 picks
# between them, and the layer check enforces for free what the closure walk
# enforced by hand. See `AGENTS.md` for what each of the four holds.
#
# So a `lua_State` or a `JSContext` in this directory does not fail a naming
# rule - it fails to compile, because neither header is on the include path.
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
		Engine::effects
		Engine::scene

		# L7, `shared`. **The same edge as `scene`, one dimension down.** A
		# script's vocabulary is the class tree, and there are two of them: a
		# `Part` comes from `scene` and a `TextButton` from `gui`. Depending on
		# one and not the other would make `.Activated` reachable only through
		# something above this module - and that something would be a second
		# path into a VM, which is exactly what `gui/Input.hpp` refuses to
		# invent when it says the join belongs to whoever owns the scripting
		# layer.
		#
		# What crosses is `gui::GuiEvent`: a kind, an entity and two points.
		# Nothing here draws, resolves a texture or knows what a canvas is.
		Engine::gui

		# L4, `shared`. **The Universe's services sit on these buses.** A script
		# reaches another world through `MessagingService` and through nothing
		# else - `world::Postbox` is the crossing, and rule 3 is why there is no
		# other one: nothing crossing a world boundary is a pointer, so a second
		# route would have to be a second copy of the same message.
		#
		# A thin view over two resources rather than a subsystem, so this buys
		# the seam and no machinery.
		Engine::world

		# L8. **`WorldRoot:Raycast` is why.** `RaycastParams` and
		# `RaycastResult` are the one part of the datatype vocabulary that is not
		# a value type - they are a *query*, and the query that answers them is
		# `physics::Raycast` against the exact shapes rather than
		# `spatial::Raycast` against the proxy boxes. A ray that reported the
		# bounding box of a rotated part would be plausible and wrong, which is
		# the failure `physics/Query.hpp` says that function exists for.
		#
		# The only edge from `script` to a simulation module, and it is one-way:
		# nothing in `physics` knows a script exists.
		Engine::physics
		Engine::parallel
)
