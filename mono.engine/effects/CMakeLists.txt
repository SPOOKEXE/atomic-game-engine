# L8. Particles, beams and trails - what a world emits rather than what it is.
#
# **`shared`, and that is load-bearing rather than tidy.** A headless server
# simulates particles for anything gameplay reads off them, and - more usefully -
# `shared` is the only tier a test suite can reach. `scene::OrderScene` makes the
# same argument in the other direction: the *pipeline* is `render`'s, and the
# arithmetic that decides what a pass is handed belongs where it can be wrong in
# a test rather than on somebody's screen.
#
# Sits beside `physics` at L8 for the same reason it does: both read `scene`'s
# components, both need `parallel` for a per-frame loop over everything, and
# neither may see the other.
#
# `parallel` is here for one loop and it is the loop the module exists for.
# `StepParticles` walks emitter blocks through `Jobs::For`, and the whole shape of
# the pool - contiguous blocks, a swap-with-last retirement, no atomics - is what
# makes that dispatch safe.
#
# Deliberately does not depend on: render, assets, or physics. A particle names
# its texture with a `core::Name` exactly as `scene::Visual` names a mesh, so
# nothing here learns what an image is; and particle collision is not implemented,
# which is what would want physics. `docs/` says so rather than the absence being
# left to be inferred.
mono_add_library(effects
	TIER shared
	DEPS
		Engine::core
		Engine::ecs
		Engine::parallel
		Engine::scene
		Engine::spatial
)
