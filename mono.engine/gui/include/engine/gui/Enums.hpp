#pragma once

// The named sets a 2D thing's properties come in.
//
// **Roblox's names and Roblox's ordinals**, both, and the second is the half
// that is easy to get wrong. Every enum here is stored as its ordinal in a
// trivially-copied component, so the *number* is the format: an `Alignment` of
// 2 has to mean `Right` in a game file this engine wrote and in one it did not.
// Reordering any of these is a format change, and there is nothing at load time
// that could catch it — a wrongly-ordered set loads cleanly and lays everything
// out somewhere plausible.
//
// That is why the values are written out below rather than left to the
// compiler, and why `gui/tests/Enums.cpp` pins the spelling of every member in
// order. `scene/Enums.hpp` makes the same argument for `NormalId` and for the
// same reason.
//
// **They live here rather than beside the component that holds them**, which is
// `scene/Enums.hpp`'s rule restated: an enum next to one struct reads as that
// struct's private business, and the moment a second thing needs an alignment —
// a list layout, a grid layout, a text label — it is either included through a
// component header that has nothing to do with it, or copied.
//
// @tier L7 · shared

#include <cstdint>

namespace engine::gui {

	// Which axis a `UDim2`'s scale resolves against.
	//
	// The default is the obvious one — X against the parent's width, Y against
	// its height. The other two exist so a square element stays square as its
	// parent changes shape, which is what `RelativeXX` on a size means: both
	// axes resolve against the parent's *width*.
	//
	// @since v0.8
	enum class SizeConstraint : uint8_t {
		RelativeXY = 0,
		RelativeXX = 1,
		RelativeYY = 2,
	};

	// Which axes an element grows along to fit what is inside it.
	//
	// @since v0.8
	enum class AutomaticSize : uint8_t {
		None = 0,
		X = 1,
		Y = 2,
		XY = 3,
	};

	// Where the border sits relative to the element's own rectangle.
	//
	// `Outline` grows the drawn box outwards, `Inset` eats into it, `Middle`
	// straddles the edge. Three answers rather than one because the choice
	// decides whether two elements sized to touch overlap by a pixel.
	//
	// @since v0.8
	enum class BorderMode : uint8_t {
		Outline = 0,
		Middle = 1,
		Inset = 2,
	};

	// How an image fills the rectangle it is drawn into.
	//
	// `Slice` is nine-slice: the four corners keep their size, the four edges
	// stretch along one axis and the middle stretches along both — which is the
	// only one of these that needs a second rectangle (`SliceCenter`) to mean
	// anything.
	//
	// @since v0.8
	enum class ScaleType : uint8_t {
		Stretch = 0,
		Slice = 1,
		Tile = 2,
		Fit = 3,
		Crop = 4,
	};

	// Horizontal placement of text inside its element.
	//
	// @since v0.8
	enum class TextXAlignment : uint8_t {
		Left = 0,
		Center = 1,
		Right = 2,
	};

	// Vertical placement of text inside its element.
	//
	// **`Top` is 0 and `Center` is 1**, which is not the same order as
	// `TextXAlignment`'s. That is Roblox's numbering and it is kept rather than
	// tidied, because the ordinal is the format — see this file's opening.
	//
	// @since v0.8
	enum class TextYAlignment : uint8_t {
		Top = 0,
		Center = 1,
		Bottom = 2,
	};

	// What happens to text too long for its element.
	//
	// @since v0.8
	enum class TextTruncate : uint8_t {
		None = 0,
		AtEnd = 1,
	};

	// The typeface a label asks for, by role.
	//
	// **By role and not by family**, which is `ui/Fonts.hpp`'s rule one module
	// over: swapping what `Code` looks like is a line in one table rather than a
	// search. The four here are the four faces already vendored for Dear ImGui,
	// because the atlas is shared — a second rasteriser over the same four files
	// would be two answers to what a glyph looks like.
	//
	// @since v0.8
	enum class FontFace : uint8_t {
		Regular = 0,
		Bold = 1,
		Italic = 2,
		Code = 3,
	};

	// Which way a list layout stacks its children.
	//
	// @since v0.8
	enum class FillDirection : uint8_t {
		Horizontal = 0,
		Vertical = 1,
	};

	// Where a layout puts its content along the horizontal axis.
	//
	// @since v0.8
	enum class HorizontalAlignment : uint8_t {
		Left = 0,
		Center = 1,
		Right = 2,
	};

	// Where a layout puts its content along the vertical axis.
	//
	// @since v0.8
	enum class VerticalAlignment : uint8_t {
		Top = 0,
		Center = 1,
		Bottom = 2,
	};

	// What order a layout visits its children in.
	//
	// @since v0.8
	enum class SortOrder : uint8_t {
		Name = 0,
		Custom = 1,
		LayoutOrder = 2,
	};

	// Which corner a grid layout fills from.
	//
	// @since v0.8
	enum class StartCorner : uint8_t {
		TopLeft = 0,
		TopRight = 1,
		BottomLeft = 2,
		BottomRight = 3,
	};

	// Which of an aspect ratio constraint's extents is the one that is obeyed.
	//
	// @since v0.8
	enum class AspectType : uint8_t {
		FitWithinMaxSize = 0,
		ScaleWithParentSize = 1,
	};

	// Which axis an aspect ratio constraint derives the other from.
	//
	// @since v0.8
	enum class DominantAxis : uint8_t {
		Width = 0,
		Height = 1,
	};

	// Which axes a scrolling frame may be scrolled along.
	//
	// @since v0.8
	enum class ScrollingDirection : uint8_t {
		X = 1,
		Y = 2,
		XY = 3,
	};

	// Whether a layer collector's `ZIndex` values are compared across the whole
	// collector or only among siblings.
	//
	// `Sibling` is Roblox's modern default and the one this engine implements:
	// a child is drawn after its parent whatever its `ZIndex`, and `ZIndex`
	// orders siblings. `Global` is the legacy behaviour where the number is
	// compared across the whole tree.
	//
	// @since v0.8
	enum class ZIndexBehavior : uint8_t {
		Global = 0,
		Sibling = 1,
	};

	// How a surface gui decides how many pixels of canvas it has.
	//
	// @since v0.8
	enum class SurfaceSizingMode : uint8_t {
		FixedSize = 0,
		PixelsPerStud = 1,
	};

	// A face of a box, for a surface gui.
	//
	// **The same six names in the same six positions as `scene::NormalId`**,
	// and this is a deliberate second declaration rather than an edge from
	// `gui` to `scene`. Both are L7 and neither may depend on the other; a UI
	// tree that had to link the 3D component set to say which face it is on
	// would be the inversion `scene/AGENTS.md` refuses in the other direction.
	//
	// `ecs::EnumTable` takes a second declaration of an existing member as
	// agreement rather than conflict, so both modules registering `NormalId`
	// is legal — and it is legal precisely as long as the *orders* match, since
	// the ordinal is what a game file carries. `gui/tests/Enums.cpp` pins this
	// list; `scene/tests/Enums.cpp` pins the other. Two tests holding one order
	// is the arrangement this repository already uses for `DefaultMaterial`.
	//
	// @since v0.8
	enum class Face : uint8_t {
		Right = 0,
		Top = 1,
		Back = 2,
		Left = 3,
		Bottom = 4,
		Front = 5,
	};

	// The member name of a value, for a registration or a log.
	//
	// **Every one of these round-trips**, unlike `scene`'s pair that are for
	// logs only: the class registration builds each enum's member list by
	// walking the range and calling these, so a name here is the name a script
	// spells and a game file carries. That is what makes the list one
	// declaration instead of two that agree until they do not.
	//@{
	const char *Describe(SizeConstraint value);
	const char *Describe(AutomaticSize value);
	const char *Describe(BorderMode value);
	const char *Describe(ScaleType value);
	const char *Describe(TextXAlignment value);
	const char *Describe(TextYAlignment value);
	const char *Describe(TextTruncate value);
	const char *Describe(FontFace value);
	const char *Describe(FillDirection value);
	const char *Describe(HorizontalAlignment value);
	const char *Describe(VerticalAlignment value);
	const char *Describe(SortOrder value);
	const char *Describe(StartCorner value);
	const char *Describe(AspectType value);
	const char *Describe(DominantAxis value);
	const char *Describe(ScrollingDirection value);
	const char *Describe(ZIndexBehavior value);
	const char *Describe(SurfaceSizingMode value);
	const char *Describe(Face value);
	//@}
}
