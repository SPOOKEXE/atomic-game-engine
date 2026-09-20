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
	// Clip data indexed for animation playback in one world.
	struct AnimationCatalogue {
		// A streamed clip and the revision last seen by the renderer.
		struct BufferedClip {
			// Source revision used to avoid reloading an unchanged clip.
			uint32_t Revision = 0;
			// Whether this entry has completed a load attempt.
			bool Loaded = false;
			// Decoded clip, absent when the source was unavailable.
			std::optional<assets::AnimationData> Clip;
		};

		// Decoded clips indexed by interned animation name.
		std::unordered_map<uint32_t, assets::AnimationData> Clips;
		// Per-entity clip load state while content arrives.
		std::unordered_map<ecs::Entity, BufferedClip> Buffers;
	};

	// Registers decoded clip data under a stable animation name.
	bool RecordAnimation(ecs::Store &store, const core::Name &name, const assets::AnimationData &clip);
	// Finds a registered clip by name, or null when absent.
	const assets::AnimationData *FindAnimation(const ecs::Store &store, const core::Name &name);

	// Samples every playing track and writes the resulting local bone poses.
	// Returns the number of Bone rows whose Transform changed.
	size_t EvaluateAnimations(ecs::Store &store);
}
