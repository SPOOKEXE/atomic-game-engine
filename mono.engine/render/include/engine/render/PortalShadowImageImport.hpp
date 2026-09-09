#pragma once

#include <engine/core/Name.hpp>
#include <engine/render/PortalShadowImage.hpp>

namespace engine::render {
	// The host authenticates the complete expected snapshot before handing over ownership.
	struct PortalShadowImageBinding {
		uint64_t World = 0;
		core::Name WorldName;
		PortalShadowSnapshot ExpectedSnapshot;
	};
}
