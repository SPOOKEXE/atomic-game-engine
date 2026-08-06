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

		# Public table APIs use asset data types.
		Engine::assets

		Engine::assets

	# Keep GPU and shader compiler types private to implementation files.
	VENDOR SDL3::SDL3 shaderc

	# Atlas headers only; no imgui library is linked.
	VENDOR_INCLUDES ${CMAKE_SOURCE_DIR}/mono.vendor/imgui
)
