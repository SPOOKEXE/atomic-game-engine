#pragma once

// The flat list of things to draw, in the order to draw them.
//
// **This is the whole seam between the tree and a renderer.** A backend takes
// this and nothing else - no store, no class table, no `UDim2`, no tree walk.
// That is what lets the same compiled list be drawn through Dear ImGui's draw
// list today and through a batched quad pipeline later without either half
// learning about the other, and it is what lets a headless test assert exactly
// what would have appeared on screen.
//
// **One struct with every field rather than a variant**, which is
// `game::PropertyValue`'s trade for the same reason: the consumers are a
// backend switching on `Kind` and a test reading fields, and a variant costs a
// visitor at both. A UI is hundreds of commands, not millions - the list is
// rebuilt only when the tree moves, so the memory is a rounding error against
// what it replaces.
//
// Everything here is in **canvas pixels**, already resolved and already
// clipped. A backend that recomputed any of it would be a second answer to
// where an element is, and the hit test reads the same numbers.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/Enums.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::gui {

	// What one command draws.
	//
	// @since v0.8
	enum class DrawKind : uint8_t {
		// A filled rectangle, optionally with rounded corners.
		Rectangle,

		// An unfilled rectangle of `Thickness` pixels - a border or a stroke.
		Outline,

		// A sampled image, stretched, sliced, tiled, fitted or cropped.
		Image,

		// A scene rendered from a `ViewportFrame`'s camera.
		Viewport,

		// A run of text inside `Bounds`, aligned by the two alignment fields.
		Text,
	};

	// One styled stretch of a text run.
	//
	// **A range over the command's own string rather than a string of its own.**
	// The backend is the only thing that can measure a glyph, so a rich-text run
	// has to reach it as one string it lays out in one pass - spans carrying
	// their own text would have to be positioned by whoever built them, using
	// compile-time estimates, and the second word would land somewhere the
	// renderer disagrees with. That is `Layout.hpp`'s one-answer rule applied to
	// markup.
	//
	// Ranges are byte offsets, in order, and do not overlap. Anything not
	// covered by a span is drawn in the command's own colour, face and size.
	//
	// @since v0.18
	struct DrawSpan {
		// The half-open byte range of `DrawCommand::Text` this styles.
		//@{
		uint32_t Begin = 0;
		uint32_t End = 0;
		//@}

		// The glyph colour over this range.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible.
		float Transparency = 0.0f;

		// The em size over this range, or 0 for the command's own.
		int32_t Size = 0;

		// Which face, by role.
		FontFace Font = FontFace::Regular;

		// Whether a rule is drawn under or through this range.
		//@{
		bool Underline = false;
		bool Strike = false;
		//@}

		// Explicit padding, for the reason every `Reserved` in this engine
		// gives - a `DrawList` reaches a benchmark's hash and a test's compare.
		uint8_t Reserved[1] = {};
	};

	// One thing to draw.
	//
	// @since v0.8
	struct DrawCommand {
		// Which of the fields below matter.
		DrawKind Kind = DrawKind::Rectangle;

		// The instance this came from.
		//
		// Not needed to draw it. It is here so a selection outline, a hit test
		// result and a "what drew this pixel" panel can all name the element,
		// which is the question every UI debugging session opens with.
		ecs::Entity Source;

		// The `LayerCollector` whose canvas these pixels belong to.
		//
		// A screen collector and a spatial collector may both use (0, 0), but
		// they do not draw in the same place. Keeping the owner on the flat
		// command lets a backend split those paths without walking the tree a
		// second time or asking the source element for its ancestors.
		ecs::Entity Collector;
		bool Spatial = false;

		// Where it goes, in canvas pixels.
		core::Rect Bounds;

		// The scissor rectangle. Already the intersection of every clipping
		// ancestor, so a backend pushes this and pops it rather than
		// maintaining a stack of its own.
		core::Rect Clip;

		// Clockwise rotation about `Bounds`'s centre, in degrees.
		float Rotation = 0.0f;

		// The colour, whatever the kind. A `Text` command's glyph colour, an
		// `Image` command's multiplier, a `Rectangle`'s fill.
		core::Color3 Tint{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible. A command at 1 is not emitted at all,
		// so a backend never has to test for it.
		float Transparency = 0.0f;

		// How thick an `Outline` is, in pixels. Ignored by every other kind.
		float Thickness = 0.0f;

		// The corner radius, in pixels. Zero is a square corner.
		float CornerRadius = 0.0f;

		// --- `Image` ---------------------------------------------------------

		// The content name to sample.
		core::Name Image;

		// How it fills `Bounds`.
		ScaleType Scale = ScaleType::Stretch;

		// How its texels are filtered. A backend that has one sampler ignores
		// this and draws linear, which is the honest degradation: an image is
		// still in the right place and is merely smoother than asked for.
		//
		// @since v0.18
		ResampleMode Resample = ResampleMode::Default;

		// The nine-slice centre, in image pixels. Only read for `Slice`.
		core::Rect SliceCenter;

		// How much the corners of a nine-slice are magnified.
		float SliceScale = 1.0f;

		// The sub-rectangle of the image to sample, in image pixels. An empty
		// rectangle means the whole image, which is what an unset
		// `ImageRectSize` resolves to.
		core::Rect Sample;

		// The tile extent in pixels, for `Tile`. Already resolved.
		core::Vector2 Tile;

		// Which fragment shader draws this image, or invalid for the
		// interface pass's own - from `Picture::Shader`, whose own header
		// carries the design.
		//
		// @since v0.18
		core::Name Shader;

		// --- `Text` ----------------------------------------------------------

		// The string.
		//
		// **Owned, which follows from `Label::Text` being owned** and is worth a
		// word because it makes a `DrawCommand` non-trivial. The list is rebuilt
		// only when the tree's hash changes - the cache is the whole of
		// `Compiled` - so this allocates on a rebuild rather than per frame, and
		// a run short enough for the small-string buffer never allocates at all.
		std::string Text;

		// The em size to draw at, already fitted and clamped. **A backend uses
		// this rather than measuring again** - see `Layout.hpp` on why there is
		// exactly one answer.
		int32_t TextSize = 0;

		// Which face, by role.
		FontFace Font = FontFace::Regular;

		// Horizontal placement inside `Bounds`.
		TextXAlignment XAlignment = TextXAlignment::Center;

		// Vertical placement inside `Bounds`.
		TextYAlignment YAlignment = TextYAlignment::Center;

		// Whether long text breaks onto further lines.
		bool Wrapped = false;

		// What happens when an unwrapped run is wider than `Bounds`.
		TextTruncate Truncate = TextTruncate::None;

		// Multiple of the em size between lines.
		float LineHeight = 1.0f;

		// The outline around every glyph. A transparency of 1 disables it.
		core::Color3 StrokeTint;
		float StrokeTransparency = 1.0f;

		// The styled ranges over `Text`, in order and non-overlapping.
		//
		// Empty for an ordinary run, which is almost all of them, and filled by
		// the rich-text parse. **A backend that ignores this draws the whole run
		// in the command's own colour and face**, which is the right degradation:
		// the words are all there and correct, only the emphasis is missing.
		//
		// @since v0.18
		std::vector<DrawSpan> Spans;

		// --- gradient --------------------------------------------------------

		// Which of `DrawList::Gradients` ramps this command, or -1 for none.
		//
		// **An index into a side table rather than the ramp itself.** A
		// `core::ColorSequence` and a `core::NumberSequence` together are over
		// six hundred bytes, and almost no command has one - carrying them
		// inline would make every rectangle in every interface pay for a feature
		// a handful of them use. The table is per frame and shared by the
		// several commands one element emits, which is also what lets a backend
		// batch by "same ramp" if it ever wants to.
		//
		// @since v0.18
		int32_t Gradient = -1;
	};

	// A colour and transparency ramp, resolved into the space it applies to.
	//
	// **Already in canvas pixels, like everything else here.** `gui::Gradient`
	// authors an offset in multiples of the parent's size and a rotation in
	// degrees; what a backend needs is the two ends of the line the ramp runs
	// along, so the compile resolves the one into the other and a backend
	// projects each point onto `Axis` and samples.
	//
	// @since v0.18
	struct DrawGradient {
		// The colour multiplied into whatever the command already draws.
		core::ColorSequence Color;

		// The transparency added to whatever the command already has.
		core::NumberSequence Transparency;

		// Where the ramp starts, in canvas pixels, before any rotation the
		// command itself carries.
		core::Vector2 Origin;

		// From `Origin` to where the ramp ends. A point's position on the ramp
		// is its projection onto this, divided by this vector's own square
		// length - which is the whole of the arithmetic a backend does.
		core::Vector2 Axis;
	};

	// A whole frame's worth, in paint order.
	//
	// @since v0.8
	struct DrawList {
		// Everything to draw, first to last. Back-to-front: a command later in
		// this list is drawn over one earlier in it, and the hit test walks it
		// backwards for exactly that reason.
		std::vector<DrawCommand> Commands;

		// The ramps `DrawCommand::Gradient` indexes. Usually empty.
		std::vector<DrawGradient> Gradients;

		// The canvas this was compiled against, in pixels.
		core::Vector2 CanvasSize;

		// How many elements were reached and drawn. Not the command count -
		// one element emits between zero and four commands.
		size_t Elements = 0;
	};
}
