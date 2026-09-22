#pragma once

// Canonical, headless text shaping over a fixed font package.
//
// A package owns validated font bytes in a declared order. Shaping produces
// logical-pixel glyph positions, visual bidi runs, and source boundaries. A
// painter consumes these values to rasterize glyphs, but never measures or
// breaks the text again.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/gui/Enums.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::gui {

	constexpr size_t MAXIMUM_SHAPED_TEXT_BYTES = 64 * 1024;
	constexpr size_t MAXIMUM_FONT_PACKAGE_BYTES = 16 * 1024 * 1024;
	constexpr size_t MAXIMUM_FONT_PACKAGE_TOTAL_BYTES = 32 * 1024 * 1024;
	constexpr size_t MAXIMUM_FONT_PACKAGE_FACES = 32;
	constexpr size_t MAXIMUM_SHAPED_GLYPHS = 16 * 1024;
	constexpr float MAXIMUM_SHAPED_PIXEL_SIZE = 4096.0f;

	// The paragraph direction supplied to Unicode bidirectional resolution.
	enum class TextDirection : uint8_t {
		Automatic,
		LeftToRight,
		RightToLeft,
	};

	// Why a shape request produced no reusable result.
	enum class TextShapeStatus : uint8_t {
		Ok,
		EmptyPackage,
		InvalidUtf8,
		TooLarge,
		NoUsableFont,
	};

	// One immutable font face from a delivered package. `Name` is content
	// identity, not a FreeType or atlas handle, so it can cross the draw seam.
	struct FontPackageFace {
		core::Name Name;
		FontFace Role = FontFace::Regular;
		std::vector<std::byte> Bytes;
	};

	// An ordered package of validated OpenType font bytes. The first face for a
	// requested role wins; remaining matching faces and then other faces form
	// deterministic fallback order.
	class FontPackage {
	  public:
		bool Add(core::Name name, FontFace role, std::span<const std::byte> bytes);

		const std::vector<FontPackageFace> &Faces() const;
		uint64_t Signature() const;

	  private:
		std::vector<FontPackageFace> Entries;
		uint64_t Fingerprint = 1469598103934665603ull;
	};

	// One glyph placed in logical pixels. `SourceByte` identifies its UTF-8
	// cluster in the original string; the index is local to `Face`.
	struct ShapedGlyph {
		core::Name Face;
		uint32_t Index = 0;
		uint32_t SourceByte = 0;
		float X = 0.0f;
		float Y = 0.0f;
		float AdvanceX = 0.0f;
		float AdvanceY = 0.0f;
		bool Synthetic = false;

		// Resolved before paint so every backend consumes the same span style.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};
		float Transparency = 0.0f;
		float PixelSize = 0.0f;
		FontFace Font = FontFace::Regular;
		bool Underline = false;
		bool Strike = false;
	};

	// A style range over the request's UTF-8 source. Kept beside the shaper,
	// rather than DrawSpan, so shared layout remains independent of a draw list.
	struct TextStyleSpan {
		uint32_t Begin = 0;
		uint32_t End = 0;
		core::Color3 Tint{1.0f, 1.0f, 1.0f};
		float Transparency = 0.0f;
		float PixelSize = 0.0f;
		FontFace Font = FontFace::Regular;
		bool Underline = false;
		bool Strike = false;
	};

	// A visually ordered run with one face and direction.
	struct ShapedRun {
		core::Name Face;
		uint32_t GlyphOffset = 0;
		uint32_t GlyphCount = 0;
		uint32_t SourceBegin = 0;
		uint32_t SourceEnd = 0;
		bool RightToLeft = false;
	};

	// One visual line in a shaped paragraph. Glyph positions remain relative to
	// this line's left edge, so every painter applies alignment once from this
	// shared answer rather than measuring the same text again.
	struct ShapedLine {
		uint32_t GlyphOffset = 0;
		uint32_t GlyphCount = 0;
		uint32_t SourceBegin = 0;
		uint32_t SourceEnd = 0;
		float Advance = 0.0f;
		float Baseline = 0.0f;
		// The line box comes from every resolved span, not from the label's
		// default size. Painters use it for vertical alignment without measuring.
		float Ascent = 0.0f;
		float Descent = 0.0f;
	};

	// A visual cursor position for one legal source boundary. A bidi boundary
	// can have more than one visual position, so callers choose the first stop
	// matching their direction policy rather than re-deriving glyph geometry.
	struct ShapedCaretStop {
		uint32_t Line = 0;
		uint32_t SourceByte = 0;
		float X = 0.0f;
	};

	// The stable answer shared by layout, interaction and a later glyph-atlas
	// painter. All byte offsets are boundaries in the original UTF-8 string.
	struct ShapedText {
		TextShapeStatus Status = TextShapeStatus::Ok;
		float Advance = 0.0f;
		std::vector<ShapedGlyph> Glyphs;
		std::vector<ShapedRun> Runs;
		std::vector<ShapedLine> Lines;
		std::vector<ShapedCaretStop> CaretStops;
		std::vector<uint32_t> GraphemeBoundaries;
		std::vector<uint32_t> LineBreakBoundaries;
	};

	struct TextShapeRequest {
		std::string_view Text;
		FontFace Role = FontFace::Regular;
		TextDirection Direction = TextDirection::Automatic;
		float PixelSize = 16.0f;
	};

	// Shapes UTF-8 with only package faces. It refuses malformed or over-limit
	// input before allocating derived runs. A missing code point becomes U+FFFD
	// in the selected package face, never a platform-font lookup.
	ShapedText ShapeText(const FontPackage &package, const TextShapeRequest &request);

	// Shapes the visual lines a label draws. Width, wrapping and ellipsis are
	// resolved here once, before a renderer or an editor painter sees the list.
	// `width <= 0` keeps explicit lines and applies no automatic wrapping.
	ShapedText LayoutText(
		const FontPackage &package,
		const TextShapeRequest &request,
		float width,
		bool wrapped,
		TextTruncate truncate,
		float lineHeight = 1.0f,
		std::span<const TextStyleSpan> styles = {}
	);
	void ApplyTextStyles(
		ShapedText &text,
		std::span<const TextStyleSpan> styles,
		core::Color3 tint,
		float transparency,
		FontFace font,
		float pixelSize
	);

	// A byte boundary resolves to the visual line and local x coordinate used by
	// a caret. The value is clamped to a legal source boundary.
	struct ShapedCaret {
		uint32_t Line = 0;
		float X = 0.0f;
	};
	ShapedCaret CaretFor(const ShapedText &text, uint32_t sourceByte);
	uint32_t SourceAt(const ShapedText &text, uint32_t line, float x);

	// A selected source range becomes one horizontal interval per visual line.
	// Painters may add their own origin and baseline, but must not derive a
	// second set of character widths.
	struct ShapedSelection {
		uint32_t Line = 0;
		float BeginX = 0.0f;
		float EndX = 0.0f;
	};
	std::vector<ShapedSelection> SelectionFor(const ShapedText &text, uint32_t begin, uint32_t end);
}
