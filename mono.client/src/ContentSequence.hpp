#pragma once

#include <engine/assets/TextureSequence.hpp>
#include <engine/scene/TextureCatalogue.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace client {
	// An animation-kind asset is a visual sequence only when its durable name
	// declares `.aseq`. Failure leaves both destinations unchanged.
	bool ReadSequenceContent(
		std::string_view name,
		std::span<const std::byte> bytes,
		engine::assets::TextureSequenceData &sequence,
		engine::scene::FlipbookFacts &facts
	);
}
