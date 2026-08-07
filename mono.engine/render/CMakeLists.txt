# L12 [client]. Shaders are compiled from this module's shaders/ directory.

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

		Engine::assets

	# Keep GPU and shader compiler types private to implementation files.
	VENDOR SDL3::SDL3 shaderc

	# Atlas headers only; no imgui library is linked.
	VENDOR_INCLUDES ${CMAKE_SOURCE_DIR}/mono.vendor/imgui
)
