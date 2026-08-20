# L5. What a shape *is*, for shapes an extent cannot describe.
#
# Convex hulls and triangle soups, and the two questions a narrow phase asks
# them: how far does this reach along a direction, and which triangles are in
# this box. Nothing here knows what an entity is, what a `Collider` component is,
# or what file the triangles came out of.
#
# **It exists at L5 because geometry has to enter the stack from below.**
# `assets::MeshData` is L8, the same height as `physics`, and both modules'
# `AGENTS.md` refuse an edge between them - two modules at one height have to
# stay disjoint, which is the rule the layer check cannot adjudicate. So a mesh
# collider cannot be `physics` reaching sideways for a mesh; it has to be a lower
# module that both can see, filled from above by whoever loads content and read
# from above by whoever solves contacts.
#
# **Below `spatial` on purpose.** The obvious next thing to want here is an index
# over a mesh's triangles, and `spatial::HashGrid` at L6 is exactly that
# structure - which is the reason this module is deliberately underneath it. A
# triangle index is an acceleration decision and belongs to `physics` at L8,
# where `spatial` is already in reach and where the measurement that justifies
# one would be taken. This module is geometry, and geometry has no schedule.
#
# `Engine::core` and nothing else. No vendor: the arithmetic is `Vector3`,
# `AABB` and a quickhull written here.
mono_add_library(collision
	TIER shared
	DEPS
		Engine::core
)
