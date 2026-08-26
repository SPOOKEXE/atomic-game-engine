# L0 core/platform and L1 core/types.
#
# core links no windowing, no graphics and no VM. The delivery service and the
# asset cooker use CFrame without acquiring either.

mono_add_library(core
	TIER shared

	# All three appear in core's public headers - spdlog in Log.hpp, glm in
	# CFrame.hpp, Tracy in Profiling.hpp - so they have to propagate. Nothing
	# else about a vendor leaves this module.
	VENDOR_PUBLIC
		spdlog::spdlog
		glm::glm
		Tracy::TracyClient

	# **core links no vendor but those three, and Crypto++ was the fourth until
	# v0.15.** It was here for `Random`'s SHA-256 and `SecureWipe`'s zeroing
	# loop, both of which are now specified algorithms written out in this
	# module - SplitMix64's finaliser and a volatile store.
	#
	# Not removed to make anything smaller. `net` and `assets` link Crypto++
	# themselves and are what put it in the client, the server and the origin,
	# and all three carry the same 43 archive members they did before;
	# `docs/retired/DEFERRED.md` D00004 has the numbers. It is removed because
	# every module in the engine links `core`, so a dependency here is a
	# dependency everywhere, and a program that wants a deterministic float
	# should not acquire a cryptography library to get one.

	DEFINES
		$<$<BOOL:${MONO_TRACY}>:ENGINE_TRACY>

		# Public, and this is the one define in the engine that has to be.
		# `ENGINE_HEAP_SCOPE` expands to a live object or to nothing depending
		# on it, so a module compiled with it off and linked against a core
		# compiled with it on would push tags nothing pops. Every module links
		# `core`, so putting it here is what makes the answer the same
		# everywhere.
		$<$<BOOL:${MONO_HEAP_PROFILE}>:MONO_HEAP_PROFILE>

		# **Public, and it has to be, for `MONO_HEAP_PROFILE`'s reason one step
		# weaker.** `ENGINE_TRACE` expands to a live statement or to a `sizeof`
		# depending on this number, so a module compiled with a different floor
		# than the one `core` was compiled with contains a different set of log
		# statements than the program thinks it does. Every module links `core`,
		# so putting it here is what makes the answer the same everywhere.
		ENGINE_LOG_COMPILED_LEVEL=${MONO_LOG_LEVEL_NUMBER}

		# Public for the same reason, and for a stronger one: `Assert::IsCompiledIn`
		# reports what this build checks, and a core that answered `true` while
		# half the modules had their asserts compiled out would be reporting
		# about a program that does not exist.
		ENGINE_ASSERTS_ENABLED=$<IF:$<BOOL:${MONO_ASSERTS}>,1,0>

		# glm settings belong here rather than in render, because CFrame is L1
		# and the two must agree about handedness and clip space. A projection
		# matrix built under a different GLM_FORCE_ is a depth buffer that is
		# subtly inverted, which reads as a z-fighting bug.
		GLM_FORCE_RADIANS
		GLM_FORCE_DEPTH_ZERO_TO_ONE   # Vulkan clip space, not OpenGL's -1..1
		GLM_ENABLE_EXPERIMENTAL       # gtx/quaternion: quatLookAt, toMat4, slerp
)

if(UNIX AND NOT APPLE)
	target_link_libraries(engine_core PRIVATE ${CMAKE_DL_LIBS})
endif()

# The version string, for `Version()` and nothing else.
#
# On the one source file rather than in DEFINES above, because DEFINES land on
# the target and every module in the engine links `core` - so a bump would
# recompile the whole engine to change one literal. Scoped here it rebuilds one
# file. `CMAKE_CONFIGURE_DEPENDS` on the root VERSION file is what makes editing
# it reach this at all; the root CMakeLists.txt sets that.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.cpp"
	PROPERTIES COMPILE_DEFINITIONS "MONO_VERSION=\"${PROJECT_VERSION}\"")
