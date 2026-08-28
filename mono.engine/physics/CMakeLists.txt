# L8. What moves, what its shape is, which shapes touch, and what that does.
#
# `shared`, and not negotiable: a headless server is the authority over where
# everything is, and a client simulating its own replica runs the same steps
# against the same components. A physics module either tier could not link would
# make the two disagree by construction.
#
# **L8 is shared with `assets`, and that is confirmed rather than inherited.**
# The height was free when this module was placed and `assets` has since taken
# it. Sharing a height is legal - `replication`,
# `input` and `render` already share L12 - and it is safe here because the two
# are disjoint: nothing in this module depends on `assets` and nothing in
# `assets` depends on this. Every dependency below is strictly lower (L7, L6,
# L3, L2, L1), so the stack is still a stack. See AGENTS.md in this directory.
#
# No vendor. The arithmetic is `Vector3`, `CFrame`, `AABB` and the glm quaternion
# `core` already propagates for `CFrame`; the six pair functions, the separating
# axis search, the manifold clip and the sequential-impulse solver are all
# written here. There is no physics library behind this module and decision 3
# says there is not going to be one.
mono_add_library(physics
	TIER shared
	DEPS
		Engine::core

		# L2. `IntegrateMotion` is `EachParallel` with a measured grain, and the
		# grain constant names `Jobs::MINIMUM_GRAINS` to explain itself.
		Engine::parallel

		# L3. The storage, the scheduler and the change channel the broad phase
		# reads to decide whether static geometry moved.
		Engine::ecs

		# L5. What a hull and a triangle soup *are*, and the support function a
		# general convex pair is built on. It exists at that height because
		# `assets::MeshData` is L8 - the same height as this module - and two
		# modules at one height have to stay disjoint, so geometry could not
		# reach a narrow phase sideways. See `collision/AGENTS.md`.
		Engine::collision

		# L6. The index and the queries over it. This module builds two grids
		# from the components at L7 and asks them what overlaps; `spatial` knows
		# nothing about either, which is why it sits below `scene` rather than
		# beside this.
		Engine::spatial

		# L7. `Transform`, `Motion`, `Collider` and the rest. The edge goes this
		# way and only this way - `scene/AGENTS.md` refuses a `physics` edge in
		# the other direction, because a component that knew about a contact
		# list would invert the stack.
		Engine::scene
)
