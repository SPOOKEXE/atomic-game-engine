#pragma once

// Passes a script adds over the lit image.
//
// A `RenderPass` under `Lighting` names a `ShaderScript` that defines
// `vec4 Pass(PassInput i)`. Passes run after deferred lighting, sorted by
// `Order`. One that is not `Composite` draws into a scratch image at `Scale`,
// which the next pass reads as `Previous`; a composite pass draws over the lit
// image by its alpha. A pass with a `Demand` runs only on frames that draw that
// surface shader.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/ClassId.hpp>
#include <engine/ecs/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	// Most passes one world runs; later ones are left out.
	inline constexpr size_t MAX_RENDER_PASSES = 8;

	// A `RenderPass` instance.
	struct RenderPass {
		core::Name Shader = {};
		// A surface shader whose draws ask for this pass, or nothing for always.
		core::Name Demand = {};
		float Scale = 1.0f;
		int32_t Order = 0;
		float Parameters[16] = {};
		uint16_t ParameterMask = 0;
		bool Composite = true;
		bool Enabled = true;
	};

	// One pass as the renderer receives it.
	struct ResolvedPass {
		core::Name Shader;
		core::Name Demand;
		float Scale = 1.0f;
		bool Composite = true;
		std::array<float, 16> Parameters{};
		uint16_t ParameterMask = 0;
	};

	// The `RenderPass` class id.
	ecs::ClassId RenderPassClass();

	// The enabled passes among `parent`'s children, in order, at most
	// `MAX_RENDER_PASSES`.
	size_t ResolveRenderPasses(
		const ecs::Store &store, ecs::Entity parent, std::array<ResolvedPass, MAX_RENDER_PASSES> &out
	);

	// Every shader the passes name, without duplicates.
	size_t DemandedPassShaders(ecs::Store &store, std::vector<core::Name> &out);
}
