#pragma once

// Portable texture references. Hosts keep asset and instance namespaces separate.
// @tier L9 · shared
#include <string>
#include <string_view>

namespace engine::imagegraph {
	// Selects whether a reference names a graph asset or a world-owned instance.
	enum class ReferenceKind {
		// A graph asset path in the `imagegraph://` namespace.
		Asset,
		// A per-world graph instance key.
		Instance
	};
	// Parsed components of an imagegraph output reference.
	struct Reference {
		// Namespace selected by the URI prefix.
		ReferenceKind Kind = ReferenceKind::Asset;
		// Asset path or instance key, depending on Kind.
		std::string Name;
		// Named graph output after the `#` separator.
		std::string Output;
	};
	// Checks for either imagegraph URI prefix; use ParseReference for full validation.
	bool IsReference(std::string_view) noexcept;
	// Checks for a portable `.atex` asset path accepted by runtime sources.
	bool IsRuntimeTexture(std::string_view) noexcept;
	// Checks canonical world-local `editable-image://` references; pixels stay separate.
	bool IsEditableImageReference(std::string_view) noexcept;
	// Checks for a portable `.aimagegraph` asset path.
	bool IsRuntimeAsset(std::string_view) noexcept;
	// Checks a bounded ASCII token used for instance keys and output names.
	bool IsReferenceToken(std::string_view) noexcept;
	// Refusal preserves out. No percent decoding or filesystem resolution occurs.
	bool ParseReference(std::string_view, Reference &out);
}
