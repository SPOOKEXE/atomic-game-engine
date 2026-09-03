#pragma once

// Device-neutral animation clip residency and pose evaluation.
// @tier L12 · client

#include <engine/assets/Animation.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	struct AnimationCatalogue {
		struct BufferedClip {
			uint32_t Revision = 0;
			bool Loaded = false;
			std::optional<assets::AnimationData> Clip;
		};

		std::unordered_map<uint32_t, assets::AnimationData> Clips;
		std::unordered_map<ecs::Entity, BufferedClip> Buffers;
	};

	bool RecordAnimation(ecs::Store &store, const core::Name &name, const assets::AnimationData &clip);
	const assets::AnimationData *FindAnimation(const ecs::Store &store, const core::Name &name);

	// Samples every playing track and writes the resulting local bone poses.
	// Returns the number of Bone rows whose Transform changed.
	size_t EvaluateAnimations(ecs::Store &store);
}
