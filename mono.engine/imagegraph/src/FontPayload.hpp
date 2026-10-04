#pragma once
#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph::detail {
	inline constexpr size_t MaximumFontGlyphs = 4096;
	uint64_t FontStorageBytes(const FontValue &, bool retained);
	uint64_t FontStorageBytes(const FontData &, bool retained);
	bool ValidFontData(const FontData &);
	bool ValidFontPayload(const FontValue &);
	// Conservative bounded check for the runtime-only font anywhere in a value tree.
	bool ContainsFontLiteral(const Value &);
}
