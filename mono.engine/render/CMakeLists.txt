# L12 [client]. The GLSL this module binds lives in Engine::resources.

set(render_vendor SDL3::SDL3 shaderc SPIRV-Tools-opt Vendor::spirv-cross Vendor::zstd freetype)
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

		# Portal image adapters use copied presentation traffic and joined world jobs.
		Engine::world

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

		# The renderer copies an installed analytical storm into its presentation
		# packet for the GPU particle field. This is a downward L12 to L8 edge.
		Engine::physics

		Engine::script

		# Public table APIs use asset data types.
		Engine::imagegraph
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

# Render adapters keep independent private conversion and file helpers.
set_target_properties(engine_render PROPERTIES UNITY_BUILD OFF)

# Compile the extension example as part of the render suite. It stays out of
# the shipped library while every API call in it remains build-checked.
if(TARGET test_render)
	target_sources(test_render PRIVATE examples/CustomNode.cpp)
	target_include_directories(test_render PRIVATE examples)
	# Headless GPU cases exercise the private backend boundary with real SDL
	# resources. The render library keeps SDL private for all public consumers;
	# its own suite is the one deliberate backend-aware caller.
	target_link_libraries(test_render PRIVATE SDL3::SDL3)
	# Portal camera images run actual character movement and obstruction queries.
	target_link_libraries(test_render PRIVATE Engine::physics)
	# The SkyGrid benchmark test loads its authored Luau scene and compiles every
	# selected ShaderScript through ShaderLibrary. This is test-only: the renderer
	# does not need example content to ship.
	target_link_libraries(test_render PRIVATE Engine::examples)
endif()

# Cook artifacts record pinned source provenance, never a fabricated compiler release number.
# Source archives without Git retain an empty identity and must supply provenance explicitly.
find_package(Git QUIET)
set(composer_cooker_identity "")
if(GIT_FOUND)
	set(composer_cooker_identity "composer-hlsl-v1")
	foreach(component IN ITEMS shaderc shaderc/third_party/glslang shaderc/third_party/spirv-tools)
		file(REAL_PATH "${CMAKE_SOURCE_DIR}/mono.vendor/${component}" composer_component_root)
		execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${composer_component_root}" rev-parse --show-toplevel
			OUTPUT_VARIABLE composer_repository_root OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE composer_repository_result ERROR_QUIET)
		if(NOT composer_repository_result EQUAL 0)
			set(composer_cooker_identity "")
			break()
		endif()
		file(REAL_PATH "${composer_repository_root}" composer_repository_root)
		if(NOT composer_repository_root STREQUAL composer_component_root)
			set(composer_cooker_identity "")
			break()
		endif()
		execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}/mono.vendor/${component}" rev-parse HEAD
			OUTPUT_VARIABLE composer_revision OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE composer_revision_result ERROR_QUIET)
		string(LENGTH "${composer_revision}" composer_revision_length)
		if(NOT composer_revision_result EQUAL 0 OR NOT composer_revision_length EQUAL 40 OR
			NOT composer_revision MATCHES "^[0-9a-f]+$")
			set(composer_cooker_identity "")
			break()
		endif()
		string(APPEND composer_cooker_identity "|${component}:${composer_revision}")
	endforeach()
endif()
target_compile_definitions(engine_render PRIVATE "ATOMIC_COMPOSER_COOKER_IDENTITY=\"${composer_cooker_identity}\"")

# The atlas experiment owns a real headless SDL device. Benchmarks normally
# measure renderer CPU preparation only, so keep this backend dependency on the
# one opt-in GPU suite rather than widening the render library's public API.
if(TARGET bench_render)
	target_link_libraries(bench_render PRIVATE SDL3::SDL3)
endif()

option(MONO_FUZZ_PORTAL_EXCHANGE "Build portal image codecs with libFuzzer and sanitizers" OFF)
if(MONO_FUZZ_PORTAL_EXCHANGE)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(FATAL_ERROR "MONO_FUZZ_PORTAL_EXCHANGE requires Clang with libFuzzer")
	endif()
	# Instrument the codec itself, including nested geometry validation.
	add_executable(fuzz_portal_exchange fuzz/PortalExchange.cpp src/PortalExchange.cpp src/PortalGeometry.cpp)
	target_include_directories(fuzz_portal_exchange PRIVATE include)
	target_link_libraries(fuzz_portal_exchange PRIVATE Engine::core Engine::assets Vendor::zstd)
	target_compile_options(fuzz_portal_exchange PRIVATE
		-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -O1 -g)
	target_link_options(fuzz_portal_exchange PRIVATE -fsanitize=fuzzer,address,undefined)
	set_target_properties(fuzz_portal_exchange PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${MONO_STAGE_ROOT}/fuzz")
endif()
