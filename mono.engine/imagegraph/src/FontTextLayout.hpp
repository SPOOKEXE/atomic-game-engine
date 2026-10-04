#pragma once
#include <engine/imagegraph/Document.hpp>

#include <string_view>

namespace engine::imagegraph::detail {
	struct FontTextLayoutOptions {
		uint8_t ChangeCase = 0, TrimType = 0;
		bool Trim = false, FullTextSize = false, Monospaced = false, SplitWord = true;
		Vector2 Range{0, 1};
		std::optional<std::string_view> ObservedCasedText;
		double Tracking = 0, LineGap = 0, MaximumLineWidth = 0;
	};
	struct FontTextLine {
		std::string Text;
		double Width = 0, Height = 0;
	};
	struct FontTextLayout {
		std::string RawText, Text;
		std::vector<FontTextLine> Lines;
		double Width = 0, Height = 0, MonoWidth = 0;
		size_t CharacterCount = 0;
	};
	const FontGlyph *FontGlyphForUnit(const FontData &, uint32_t);
	double FontUnitAdvance(const FontData &, uint32_t);
	double FontScalarAdvance(const FontData &, uint32_t);
	std::optional<uint64_t> FontTextNativeMeasurementAdmissionBytes(const FontData &, size_t textBytes);
	// Uses observed coverage metrics without locale state. Source ASCII casing is explicit; Unicode
	// casing follows the selected context; exact measurements override native font sizing.
	Status BuildFontTextLayout(
		const FontData &,
		std::string_view,
		const FontTextLayoutOptions &,
		uint64_t maximumBytes,
		FontTextLayout &,
		std::string &failure
	);
}
