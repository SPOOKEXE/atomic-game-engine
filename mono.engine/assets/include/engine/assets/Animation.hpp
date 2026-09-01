#pragma once

// Baked joint animation data. Runtime handlers sample this format; foreign
// animation containers are converted before they reach a running game.
// @tier L8 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>

#include <cstdint>
#include <vector>

namespace engine::assets {

	struct AnimationKeyframe {
		float Time = 0.0f;
		core::CFrame Transform;
	};

	struct AnimationChannel {
		uint16_t Joint = 0;
		std::vector<AnimationKeyframe> Keys;
	};

	struct AnimationData {
		float Duration = 0.0f;
		std::vector<AnimationChannel> Channels;

		bool IsValid() const;
	};

	class Animation {
	  public:
		static constexpr uint32_t MAGIC = 0x314E4141; // "AAN1"
		static constexpr uint16_t VERSION = 1;
		static constexpr uint16_t MAXIMUM_JOINTS = 256;
		static constexpr uint32_t MAXIMUM_CHANNELS = 256;
		static constexpr uint32_t MAXIMUM_KEYS_PER_CHANNEL = 1u << 20u;
		static constexpr uint32_t MAXIMUM_KEYS = 4u << 20u;

		static bool Write(core::ByteWriter &writer, const AnimationData &data);
		static bool Read(core::ByteReader &reader, AnimationData &out);
	};
}
