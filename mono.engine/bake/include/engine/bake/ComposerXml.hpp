#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace engine::bake {
	struct ComposerXmlNode {
		std::string Type;
		std::vector<std::pair<std::string, std::string>> Attributes;
		std::optional<std::string> Text;
		std::vector<ComposerXmlNode> Children;
	};
	struct ComposerXml {
		ComposerXmlNode Root;
		std::optional<ComposerXmlNode> Prolog;
	};
	// Native SnapFromXML root shape, preserving raw entity text without external resource resolution.
	bool ReadComposerXml(
		std::string_view text,
		ComposerXml &output,
		std::string &failure,
		uint64_t maximumBytes = 64ull * 1024 * 1024
	);
}
