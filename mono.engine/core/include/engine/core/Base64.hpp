#pragma once

// Standard base64 (RFC 4648, with `=` padding) for binary carried in text,
// such as an EditableMesh or EditableImage saved inside a world document.
//
// @tier L1 · shared

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::core {
	// Encodes bytes as base64 text.
	std::string EncodeBase64(std::span<const std::byte> bytes);

	// Decodes base64 text. False, with `out` unspecified, on any character
	// outside the alphabet, misplaced padding or a length not a multiple of 4.
	bool DecodeBase64(std::string_view text, std::vector<std::byte> &out);
}
