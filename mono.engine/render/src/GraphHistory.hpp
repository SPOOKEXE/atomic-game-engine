#pragma once

// The identity and ownership rules for graph resources retained between views.

#include <engine/graph/Schedule.hpp>
#include <engine/render/PresentationDamage.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace engine::render {

	inline uint64_t GraphHistorySignature(
		uint64_t contentSignature,
		const scene::CameraMatrices &matrices,
		uint32_t width,
		uint32_t height
	) {
		uint64_t signature = contentSignature;
		for (const glm::mat4 *matrix : {&matrices.ViewProjection, &matrices.Projection})
			for (size_t column = 0; column < 4; column++)
				for (size_t row = 0; row < 4; row++)
					signature = scene::MixSignature(signature, std::bit_cast<uint32_t>((*matrix)[column][row]));
		signature = scene::MixSignature(signature, width);
		return scene::MixSignature(signature, height);
	}

	// ContentSignature deliberately omits these changing scene inputs.
	inline bool GraphHistoryReadable(const PresentationDamage &damage) {
		return !damage.Scene && !damage.Objects && !damage.Environment && !damage.Viewport && !damage.Portals;
	}

	inline uint64_t GraphHistoryOwner(graph::NodeScope scope, size_t view, uint64_t world) {
		return scope == graph::NodeScope::View   ? static_cast<uint64_t>(view)
			 : scope == graph::NodeScope::World ? world
										: 0;
	}
}
