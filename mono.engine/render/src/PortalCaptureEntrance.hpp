#pragma once

#include <engine/render/PortalExchange.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <optional>
#include <span>

namespace engine::render {
	// Ambiguous coincident mouths remain visible rather than hiding unrelated geometry.
	std::optional<int16_t>
	ResolvePortalEntrance(const PortalImageEntrance &entrance, std::span<const scene::PortalSeam> seams);
}
