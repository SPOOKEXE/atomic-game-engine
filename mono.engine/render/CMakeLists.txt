# L12 [client]. The GLSL this module binds lives in Engine::resources.

mono_add_library(render
	TIER client
	DEPS
		Engine::core

		# Scene data and culling decisions are shared with headless hosts.
		Engine::graph

		# Shared interface draw data; the client does not link Engine::ui.
		Engine::gui

		Engine::scene

		# `render::ParticleBatch` carries a span of `effects::ParticleInstance`,
		# which is the whole point of the type: the particle stream crosses from
		# simulation to presentation without being copied or converted. That makes
		# it a public dependency in the same way `assets` is — the payload type has
		# to propagate to whoever fills a batch.
		#
		# L8 is below L12 and `effects` is `shared`, so the edge goes downward and
		# the tier check has nothing to say about it.
		Engine::effects

		# Public table APIs use asset data types.
		Engine::assets

		# The built-in SPIR-V, and the one name that says where it is staged.
		#
		# **A link edge for files rather than for symbols, and it has to be one.**
		# `mono_add_program` stages the shaders of every module a program links,
		# so a renderer that read `shaders/resources/` without linking the module
		# that owns it would build, link, stage nothing and fail at pipeline
		# creation on a machine where a full build had not happened to run first.
		Engine::resources

	# Keep GPU and shader compiler types private to implementation files.
	VENDOR SDL3::SDL3 shaderc

	# Atlas headers only; no imgui library is linked.
	VENDOR_INCLUDES ${CMAKE_SOURCE_DIR}/mono.vendor/imgui
)
