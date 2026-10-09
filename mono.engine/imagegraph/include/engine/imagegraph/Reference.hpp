#pragma once

// Portable texture references. Hosts keep asset and instance namespaces separate.
// @tier L9 · shared
#include <string>
#include <string_view>

namespace engine::imagegraph {
	enum class ReferenceKind { Asset, Instance };
	struct Reference {
		ReferenceKind Kind = ReferenceKind::Asset;
		std::string Name;
		std::string Output;
	};
	bool IsReference(std::string_view) noexcept;
	bool IsRuntimeTexture(std::string_view) noexcept;
	bool IsRuntimeAsset(std::string_view) noexcept;
	bool IsReferenceToken(std::string_view) noexcept;
	// Refusal preserves out. No percent decoding or filesystem resolution occurs.
	bool ParseReference(std::string_view, Reference &out);
}
