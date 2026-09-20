#pragma once

#include <engine/core/Name.hpp>
#include <engine/render/PortalShadowImage.hpp>

namespace engine::render {
	// The host authenticates the complete expected snapshot before handing over ownership.
	struct PortalShadowImageBinding {
		// Process-local world handle that owns this expected shadow request.
		uint64_t World = 0;
		// Stable world identity used to reject replies for a different world.
		core::Name WorldName;
		// Fully authenticated snapshot the incoming manifest and tiles must match.
		PortalShadowSnapshot ExpectedSnapshot;
	};
}
