# L12 [client]. The GLSL this module binds lives in Engine::resources.

set(render_vendor SDL3::SDL3 shaderc SPIRV-Tools-opt Vendor::spirv-cross)
if(APPLE)
	# SDL loads this dynamically. Naming it here lets mono_add_program stage it
	# for every executable that actually links the renderer.
	list(APPEND render_vendor MoltenVK::Runtime)
endif()

mono_add_library(render
	TIER client
	DEPS
		Engine::core
		Engine::parallel

		# WorldPresentation is the shared world-to-renderer boundary.
		Engine::ecs

		# Scene data and culling decisions are shared with headless hosts.
		Engine::graph

		# Shared interface draw data; the client does not link Engine::ui.
		Engine::gui

		Engine::scene

		# `render::ParticleBatch` names the effects block, spawn state and runtime
		# rows. The renderer owns the particle pool and steps it, so those layouts
		# have to be the ones simulation writes. That makes it a public dependency
		# in the same way `assets` is - the payload
		# type has to propagate to whoever fills a batch.
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

		# SPIR-V to MSL, for the half a build step cannot do.
		#
		# **A module rather than a function in here, because there are two
		# callers.** `mono.tools/shadercross` translates the built-in shaders
		# during the build and this translates a `ShaderScript` while the engine
		# runs; one implementation is what stops the two from disagreeing about
		# which texture is `[[texture(0)]]`, which on macOS is a surface sampling
		# somebody else's map and on Linux is nothing at all.
		Engine::msl

	# Keep GPU and shader compiler types private to implementation files.
	VENDOR ${render_vendor}

	# Atlas headers only; no imgui library is linked.
	VENDOR_INCLUDES
		${CMAKE_SOURCE_DIR}/mono.vendor/imgui
		${CMAKE_SOURCE_DIR}/mono.vendor/sdl/src/video/khronos
)

# Compile the extension example as part of the render suite. It stays out of
# the shipped library while every API call in it remains build-checked.
if(TARGET test_render)
	target_sources(test_render PRIVATE examples/CustomNode.cpp)
	target_include_directories(test_render PRIVATE examples)
	# Headless GPU cases exercise the private backend boundary with real SDL
	# resources. The render library keeps SDL private for all public consumers;
	# its own suite is the one deliberate backend-aware caller.
	target_link_libraries(test_render PRIVATE SDL3::SDL3)
endif()
