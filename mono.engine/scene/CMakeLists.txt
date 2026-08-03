# L7. What a thing in a world *is* — the component set both programs share.
#
# The ECS is storage and must not know what a Transform is; a program owning its
# own Transform is two definitions of one fact, which is the debt this module
# exists to pay off. So the components live one layer above the storage and
# below everything that reads them.
#
# `shared`, and that is the load-bearing half. A server-tier host writes a
# `DrawInstance` and a client-tier consumer reads it, so the payload cannot be
# either tier — see AGENTS.md on why no device type may appear here.
#
# No vendor. glm reaches the resolved camera matrices through `Engine::core`,
# which already propagates it for `CFrame`; nothing else here needs one.
#
# `spatial` is here for one type. `Collider` carries the layer it is on and the
# set it collides with, and those are the same two numbers a query takes — so
# they are `spatial::LayerMask` on both sides rather than a `uint32_t` here and
# a named type there. L6 is below L7, so the edge goes downward; it buys nothing
# else and must not start to.
#
# Deliberately does not depend on: physics, render or world. AGENTS.md in this
# directory gives the reason for each, and each of them is a convention the tier
# check cannot catch because they are all `shared` too.
mono_add_library(scene
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::spatial
)
