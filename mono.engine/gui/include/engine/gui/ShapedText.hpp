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

	// Largest UTF-8 source accepted by one shape request.
	constexpr size_t MAXIMUM_SHAPED_TEXT_BYTES = 64 * 1024;
	// Largest encoded font face in one package.
	constexpr size_t MAXIMUM_FONT_PACKAGE_BYTES = 16 * 1024 * 1024;
	// Largest combined encoded font package.
	constexpr size_t MAXIMUM_FONT_PACKAGE_TOTAL_BYTES = 32 * 1024 * 1024;
	// Largest number of faces in one package.
	constexpr size_t MAXIMUM_FONT_PACKAGE_FACES = 32;
	// Largest number of produced shaped glyphs.
	constexpr size_t MAXIMUM_SHAPED_GLYPHS = 16 * 1024;
	// Largest accepted text em size in logical pixels.
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
		// Content identity of this face.
		core::Name Name;
		// Font role selected by this face.
		FontFace Role = FontFace::Regular;
		// Validated OpenType font bytes.
		std::vector<std::byte> Bytes;
	};

	// An ordered package of validated OpenType font bytes. The first face for a
	// requested role wins; remaining matching faces and then other faces form
	// deterministic fallback order.
	class FontPackage {
	  public:
		// Adds one validated face within package limits.
		bool Add(core::Name name, FontFace role, std::span<const std::byte> bytes);

		// Package faces in deterministic fallback order.
		const std::vector<FontPackageFace> &Faces() const;
		// Stable content fingerprint of the package.
		uint64_t Signature() const;

	  private:
		std::vector<FontPackageFace> Entries;
		uint64_t Fingerprint = 1469598103934665603ull;
	};

	// One glyph placed in logical pixels. `SourceByte` identifies its UTF-8
	// cluster in the original string; the index is local to `Face`.
	struct ShapedGlyph {
		// Content identity of the glyph's font face.
		core::Name Face;
		// Glyph index within Face.
		uint32_t Index = 0;
		// UTF-8 byte offset of the source cluster.
		uint32_t SourceByte = 0;
		// Horizontal glyph origin in logical pixels.
		float X = 0.0f;
		// Vertical glyph origin in logical pixels.
		float Y = 0.0f;
		// Horizontal advance in logical pixels.
		float AdvanceX = 0.0f;
		// Vertical advance in logical pixels.
		float AdvanceY = 0.0f;
		// Whether shaping inserted this glyph without source text.
		bool Synthetic = false;

		// Resolved before paint so every backend consumes the same span style.
		// Resolved glyph tint.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};
		// Resolved glyph transparency.
		float Transparency = 0.0f;
		// Resolved glyph em size.
		float PixelSize = 0.0f;
		// Resolved glyph font role.
		FontFace Font = FontFace::Regular;
		// Whether to draw an underline.
		bool Underline = false;
		// Whether to draw a strike-through.
		bool Strike = false;
	};

	// A style range over the request's UTF-8 source. Kept beside the shaper,
	// rather than DrawSpan, so shared layout remains independent of a draw list.
	struct TextStyleSpan {
		// First styled UTF-8 byte offset.
		uint32_t Begin = 0;
		// One past the final styled UTF-8 byte.
		uint32_t End = 0;
		// Glyph tint over this range.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};
		// Glyph transparency over this range.
		float Transparency = 0.0f;
		// Em size over this range, or zero for the request size.
		float PixelSize = 0.0f;
		// Requested font role over this range.
		FontFace Font = FontFace::Regular;
		// Whether to draw a rule beneath this range.
		bool Underline = false;
		// Whether to draw a rule through this range.
		bool Strike = false;
	};

	// A visually ordered run with one face and direction.
	struct ShapedRun {
		// Content identity of the face that shaped this run.
		core::Name Face;
		// First glyph index in ShapedText::Glyphs.
		uint32_t GlyphOffset = 0;
		// Number of glyphs in this run.
		uint32_t GlyphCount = 0;
		// First source UTF-8 byte covered by this run.
		uint32_t SourceBegin = 0;
		// One past the final source UTF-8 byte covered by this run.
		uint32_t SourceEnd = 0;
		// Whether visual glyph order runs right to left.
		bool RightToLeft = false;
	};

	// One visual line in a shaped paragraph. Glyph positions remain relative to
	// this line's left edge, so every painter applies alignment once from this
	// shared answer rather than measuring the same text again.
	struct ShapedLine {
		// First glyph index in ShapedText::Glyphs.
		uint32_t GlyphOffset = 0;
		// Number of glyphs on this line.
		uint32_t GlyphCount = 0;
		// First source UTF-8 byte on this line.
		uint32_t SourceBegin = 0;
		// One past the final source UTF-8 byte on this line.
		uint32_t SourceEnd = 0;
		// Horizontal line advance in logical pixels.
		float Advance = 0.0f;
		// Baseline offset from the line's top.
		float Baseline = 0.0f;
		// The line box comes from every resolved span, not from the label's
		// default size. Painters use it for vertical alignment without measuring.
		float Ascent = 0.0f;
		// Distance below the baseline in logical pixels.
		float Descent = 0.0f;
	};

	// A visual cursor position for one legal source boundary. A bidi boundary
	// can have more than one visual position, so callers choose the first stop
	// matching their direction policy rather than re-deriving glyph geometry.
	struct ShapedCaretStop {
		// Visual line containing this stop.
		uint32_t Line = 0;
		// Source UTF-8 boundary represented by this stop.
		uint32_t SourceByte = 0;
		// Horizontal stop coordinate in logical pixels.
		float X = 0.0f;
	};

	// The stable answer shared by layout, interaction and a later glyph-atlas
	// painter. All byte offsets are boundaries in the original UTF-8 string.
	struct ShapedText {
		// Outcome of the shaping request.
		TextShapeStatus Status = TextShapeStatus::Ok;
		// Maximum horizontal advance across visual lines.
		float Advance = 0.0f;
		// Glyphs in visual line order.
		std::vector<ShapedGlyph> Glyphs;
		// Directional face runs over Glyphs.
		std::vector<ShapedRun> Runs;
		// Visual lines in layout order.
		std::vector<ShapedLine> Lines;
		// Legal visual cursor positions.
		std::vector<ShapedCaretStop> CaretStops;
		// Legal grapheme source boundaries.
		std::vector<uint32_t> GraphemeBoundaries;
		// Legal line-break source boundaries.
		std::vector<uint32_t> LineBreakBoundaries;
	};

	// Inputs that select one canonical shaped text result.
	struct TextShapeRequest {
		// UTF-8 source text to shape.
		std::string_view Text;
		// Preferred font role.
		FontFace Role = FontFace::Regular;
		// Paragraph direction policy.
		TextDirection Direction = TextDirection::Automatic;
		// Requested em size in logical pixels.
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
	// Applies source style ranges and default glyph styling to a shaped result.
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
		// Visual line containing the caret.
		uint32_t Line = 0;
		// Horizontal caret coordinate in logical pixels.
		float X = 0.0f;
	};
	// Locates the visual caret for a source UTF-8 boundary.
	ShapedCaret CaretFor(const ShapedText &text, uint32_t sourceByte);
	// Locates the source UTF-8 boundary nearest a visual coordinate.
	uint32_t SourceAt(const ShapedText &text, uint32_t line, float x);

	// A selected source range becomes one horizontal interval per visual line.
	// Painters may add their own origin and baseline, but must not derive a
	// second set of character widths.
	struct ShapedSelection {
		// Visual line containing this interval.
		uint32_t Line = 0;
		// Left interval edge in logical pixels.
		float BeginX = 0.0f;
		// Right interval edge in logical pixels.
		float EndX = 0.0f;
	};
	// Converts a source range into visual selection intervals.
	std::vector<ShapedSelection> SelectionFor(const ShapedText &text, uint32_t begin, uint32_t end);
}
