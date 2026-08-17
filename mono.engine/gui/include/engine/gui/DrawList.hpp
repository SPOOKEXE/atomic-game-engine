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
	};

	// A whole frame's worth, in paint order.
	//
	// @since v0.8
	struct DrawList {
		// Everything to draw, first to last. Back-to-front: a command later in
		// this list is drawn over one earlier in it, and the hit test walks it
		// backwards for exactly that reason.
		std::vector<DrawCommand> Commands;

		// The canvas this was compiled against, in pixels.
		core::Vector2 CanvasSize;

		// How many elements were reached and drawn. Not the command count -
		// one element emits between zero and four commands.
		size_t Elements = 0;
	};
}
