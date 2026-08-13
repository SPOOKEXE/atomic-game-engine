#pragma once

// The one place this module decides what a name's extension is.
//
// Both halves of the importer need the answer — `ModelFormatOfName` because OBJ
// has no signature and a `.gltf` is JSON, `ImageFormatOfName` because SVG is XML
// — and two copies of a rule about where a dot counts is exactly the drift
// `assets::KindOfName` is one function for.

#include <algorithm>
#include <string>
#include <string_view>

namespace engine::bake {

	// A lowercase copy of a name's extension, or an empty string when it has
	// none.
	//
	// The rule is `assets::KindOfName`'s, including that a dot inside a
	// directory component is not an extension: `v1.2/rock` has none, and reading
	// `2/rock` as one would classify an asset by whatever that happened to
	// match.
	//
	// @param name A file name or path, with forward slashes.
	// @return The extension without its dot, lowercased.
	inline std::string ExtensionOf(std::string_view name) {
		const size_t dot = name.find_last_of('.');
		if (dot == std::string_view::npos || dot + 1 >= name.size()) {
			return {};
		}
		if (name.find('/', dot) != std::string_view::npos) {
			return {};
		}

		std::string extension(name.substr(dot + 1));
		std::transform(extension.begin(), extension.end(), extension.begin(), [](char value) {
			return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
		});
		return extension;
	}
}
