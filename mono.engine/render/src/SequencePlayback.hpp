#pragma once

#include <engine/assets/TextureSequence.hpp>
#include <engine/core/Name.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace engine::render {
	// CPU sequence admission and fair publication scheduling. The callback owns
	// the device upload; this class never retains a GPU handle or a clock.
	class SequencePlayback {
	  public:
		// Holds two maximum-size sequences during a last-good replacement.
		static constexpr size_t MAXIMUM_RETAINED_BYTES =
			2 * assets::TextureSequence::MAXIMUM_PIXEL_BYTES +
			4 * assets::TextureSequence::MAXIMUM_FRAMES * sizeof(float);
		static constexpr size_t FRAME_UPLOAD_BUDGET = 8u * 1024u * 1024u;
		static constexpr size_t MAXIMUM_ENTRIES = 4096;

		using Publish = std::function<bool(
			core::Name name, core::Name owner, const assets::TextureSequenceData &sequence, uint32_t frame
		)>;

		explicit SequencePlayback(size_t maximumBytes = MAXIMUM_RETAINED_BYTES)
			: MaximumBytes(maximumBytes) {}

		// A replacement remains pending until its first frame publishes. Failure
		// keeps the previous admitted sequence and its last GPU frame.
		bool Admit(core::Name name, core::Name owner, assets::TextureSequenceData sequence);
		size_t Advance(double seconds, size_t byteBudget, const Publish &publish);
		bool Drop(core::Name name, core::Name owner);
		size_t DropOwner(core::Name owner);
		bool Contains(core::Name name, core::Name owner) const;
		uint64_t PublishedSignature() const;
		size_t RetainedBytes() const {
			return HeldBytes;
		}

	  private:
		struct Sequence {
			assets::TextureSequenceData Data;
			std::vector<float> CumulativeEnds;
			uint32_t PublishedFrame = 0;
			bool Published = false;
		};
		struct Entry {
			core::Name Name;
			core::Name Owner;
			std::optional<Sequence> Active;
			std::optional<Sequence> Pending;
		};

		static uint64_t Key(core::Name name, core::Name owner);
		static size_t BytesOf(const Sequence &sequence);
		std::map<uint64_t, Entry> Entries;
		size_t HeldBytes = 0;
		size_t MaximumBytes = MAXIMUM_RETAINED_BYTES;
		uint64_t LastAttempted = 0;
	};
}
