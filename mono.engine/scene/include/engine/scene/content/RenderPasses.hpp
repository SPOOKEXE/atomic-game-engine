#pragma once

// Passes a script adds over the lit image.
//
// A `RenderPass` under `Lighting` names a `ShaderScript` that defines
// `vec4 Pass(PassInput i)`. `Stage` picks where in the frame it runs: after
// deferred lighting, or after transparent draws, over the scene before tone
// mapping. Within a stage passes run by `Order`. One that is not `Composite`
// draws into a scratch image at `Scale`, which the next pass reads as
// `Previous`; with an `Output` name it draws into that image instead, which any
// later pass in the frame reads through `#pragma image name`. A composite pass
// draws over the scene by its alpha. A shader that defines `void
// Compute(PassInput i, ivec2 pixel)` instead runs as a compute dispatch over its
// image and may `Store` to any pixel of it; it is never composite. A pass with a
// `Demand` runs only on frames that draw that surface shader.
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

	// Where in the frame a pass runs.
	enum class PassStage : uint8_t {
		// Over the lit image, after deferred lighting.
		Lighting,
		// Over the scene after transparent draws, before tone mapping.
		Transparent,
	};

	// A `RenderPass` instance.
	struct RenderPass {
		core::Name Shader = {};
		// A surface shader whose draws ask for this pass, or nothing for always.
		core::Name Demand = {};
		// A named image a non-composite pass draws into, or nothing for `Previous`.
		core::Name Output = {};
		PassStage Stage = PassStage::Lighting;
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
		core::Name Output;
		PassStage Stage = PassStage::Lighting;
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
