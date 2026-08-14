# L6. Where things are, as far as anything asking is concerned.
#
# A uniform hash grid over `{id, AABB, layers}` and the queries that read it.
# Its own module rather than a corner of `physics` because render culling wants
# the same structure and should not have to link a solver to get it - which is
# also why it sits below `scene` rather than beside it.
#
# `Engine::core` and nothing else, and that single line is the module's main
# invariant. This layer does not know what an entity is: an id here is whatever
# 64-bit number the caller put in. An edge to `ecs` would be legal by tier and
# legal by height, so nothing in the build would catch it - AGENTS.md in this
# directory is the only thing that does.
#
# No vendor. The arithmetic is `Vector3`, `AABB` and `Ray` out of core.
mono_add_library(spatial
	TIER shared
	DEPS
		Engine::core
)
