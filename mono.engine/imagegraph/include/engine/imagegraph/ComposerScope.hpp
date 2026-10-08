#pragma once

#include <cstdint>
#include <string_view>

namespace engine::imagegraph {
	struct Node;
	struct Diagnostic;
	enum class Status : uint8_t;

	// grug keep host scope separate from saved graphs and reusable executors.
	enum class ComposerScope : uint8_t { Unrestricted, ImageOnly };
	enum class ComposerDomain : uint8_t { Image, Mesh3D, Audio, Video };

	ComposerDomain ComposerNodeDomain(std::string_view type);
	ComposerDomain ComposerNodeDomain(const Node &node);
	bool ComposerNodeEnabled(std::string_view type, ComposerScope scope);
	bool ComposerNodeEnabled(const Node &node, ComposerScope scope);
	Status CheckComposerNodeScope(const Node &node, ComposerScope scope, Diagnostic &diagnostic);
	bool ComposerExportEnabled(std::string_view extension, ComposerScope scope);
}
