#pragma once

// Baked joint animation data. Runtime handlers sample this format; foreign
// animation containers are converted before they reach a running game.
// @tier L8 · shared

#include <engine/core/Bytes.hpp>
#include <engine/core/types/CFrame.hpp>

#include <cstdint>
#include <vector>

namespace engine::assets {

	// Animation Keyframe declaration.
	struct AnimationKeyframe {
		// Keyframe time in seconds.
		float Time = 0.0f;
		// Coordinate frame for transform.
		core::CFrame Transform;
	};

	// Animation Channel declaration.
	struct AnimationChannel {
		// Skeleton joint index driven by this channel.
		uint16_t Joint = 0;
		// Keyframes in increasing animation time order.
		std::vector<AnimationKeyframe> Keys;
	};

	// Animation Data declaration.
	struct AnimationData {
		// Animation duration in seconds.
		float Duration = 0.0f;
		// Number of audio channels.
		std::vector<AnimationChannel> Channels;

		// Verifies channel times, values, and interpolation data form a usable clip.
		bool IsValid() const;
	};

	// Animation declaration.
	class Animation {
	  public:
		static constexpr uint32_t MAGIC = 0x314E4141; // "AAN1"
		// Version used by this object.
		static constexpr uint16_t VERSION = 1;
		// Configured limit for maximumjoints.
		static constexpr uint16_t MAXIMUM_JOINTS = 256;
		// Configured limit for maximumchannels.
		static constexpr uint32_t MAXIMUM_CHANNELS = 256;
		// Configured limit for maximumkeysperchannel.
		static constexpr uint32_t MAXIMUM_KEYS_PER_CHANNEL = 1u << 20u;
		// Configured limit for maximumkeys.
		static constexpr uint32_t MAXIMUM_KEYS = 4u << 20u;

		// Writes a bounded animation payload in the asset binary format.
		static bool Write(core::ByteWriter &writer, const AnimationData &data);
		// Reads and validates one bounded animation payload from the asset binary format.
		static bool Read(core::ByteReader &reader, AnimationData &out);
	};
}
