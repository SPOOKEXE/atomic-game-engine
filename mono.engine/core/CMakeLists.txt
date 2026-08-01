# L0 core/platform and L1 core/types.
#
# core links no windowing, no graphics and no VM. The delivery service and the
# asset cooker use CFrame without acquiring either.

mono_add_library(core
	TIER shared

	# All three appear in core's public headers — spdlog in Log.hpp, glm in
	# CFrame.hpp, Tracy in Profiling.hpp — so they have to propagate. Nothing
	# else about a vendor leaves this module.
	VENDOR_PUBLIC
		spdlog::spdlog
		glm::glm
		Tracy::TracyClient

	# SHA-256 behind Random.hpp, which needs a value that is identical on every
	# machine rather than merely random. VENDOR, not VENDOR_PUBLIC: Random.hpp
	# exposes uint32_t and float and no Crypto++ type, so nothing about it
	# leaves this module and no other module pays for the header.
	#
	# Everything links core, so this is the widest a vendor gets here. It costs
	# other modules nothing to compile, and the linker drops what nobody calls —
	# the same way shaderc is linked by render and absent from the client, as
	# THIRD_PARTY_NOTICES.md records.
	VENDOR
		Vendor::cryptopp

	DEFINES
		$<$<BOOL:${MONO_TRACY}>:ENGINE_TRACY>

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
