# L9. What is drawn, decided before anything is bound.
#
# **The half of rendering that is arithmetic.** `render` owns the device: the
# pipelines, the buffers, the shaders, the swapchain. This owns the decisions
# that come before any of that - which instances a view can see, which order
# they go in, and what the passes are. Splitting them is what makes the
# decisions testable at all: a frustum test is six dot products and can be
# asserted against; a draw call needs a GPU and can only be looked at.
#
# `shared` rather than `client`, and that is the load-bearing choice. A
# server-tier host publishes a view for a world it hosts - `world::ViewChannel`
# is the seam - and culling that view is the same arithmetic wherever it runs.
# A `client` tier here would mean a headless host had to ship its whole draw
# list across a process boundary for the far side to throw most of it away.
#
# Above `scene` at L7 because a `DrawInstance` is what it culls. Below `render`,
# which consumes what it decides.
#
# No vendor beyond what `core` already pulls: the arithmetic is `glm` matrices
# and `core::AABB`, both of which arrive through `Engine::core`.
mono_add_library(graph
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::parallel

		# `scene` for `DrawInstance` - the thing being culled - and for
		# `ResolveCamera`, which is the one place the engine decides what a
		# camera's matrices are. A frustum built from a second answer to that
		# question would disagree with what was actually projected, and the
		# symptom is geometry popping at the screen edge on one machine and not
		# another.
		Engine::scene
)
