#include "ContentSequence.hpp"

#include <engine/assets/ContentForm.hpp>
#include <engine/core/Bytes.hpp>

#include <utility>

namespace client {
	bool ReadSequenceContent(
		std::string_view name,
		std::span<const std::byte> bytes,
		engine::assets::TextureSequenceData &sequence,
		engine::scene::FlipbookFacts &facts
	) {
		if (engine::assets::FormOfName(name) != engine::assets::ContentForm::ASeq) return false;
		engine::assets::TextureSequenceData decoded;
		engine::core::ByteReader reader(bytes);
		if (!engine::assets::TextureSequence::Read(reader, decoded) || decoded.FrameDurations.size() <= 256)
			return false;
		engine::scene::FlipbookFacts recorded;
		recorded.Frames = static_cast<uint16_t>(decoded.FrameDurations.size());
		recorded.FrameDurations = decoded.FrameDurations;
		sequence = std::move(decoded);
		facts = std::move(recorded);
		return true;
	}
}
