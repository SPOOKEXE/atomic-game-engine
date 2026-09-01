#pragma once

// The editable JSON view of one script-codec value.
//
// Every value carries an explicit type, including maps and arrays. That keeps
// an authored map from colliding with the representation of Vector3 or CFrame.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studio {
	// Decodes script bytes and writes indented typed JSON.
	bool DatasetValueToText(std::span<const std::byte> bytes, std::string &text, std::string &error);

	// Parses typed JSON and encodes one script value.
	bool DatasetValueFromText(std::string_view text, std::vector<std::byte> &bytes, std::string &error);
}
