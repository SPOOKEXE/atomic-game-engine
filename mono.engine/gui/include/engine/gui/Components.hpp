#pragma once

// What a 2D thing in a game *is* — the component set both halves share.
//
// `scene` answers the same question for a part and this is that file's argument
// applied one dimension down. A server authors a `ScreenGui` and replicates it,
// so a type only a client could name would be a type only a client could build
// one out of. Nothing here needs a device, a font atlas or a swapchain: an
// element is a position, a size, some colours and a string, and every one of
// those is a value a headless host can produce and mean.
//
// **The split is by what is iterated, not by what a class has.** `Element` is
// on every `GuiObject` and is read by the layout pass; `Background` is read by
// the draw pass; `Label` is read only by the text run. A `TextButton` carries
// all three and a `Frame` carries two, so the query for "everything that lays
// out" never loads a font size it does not use. That is `ecs/AGENTS.md`'s rule
// and it is the same one `scene` applies to `Motion` and `RigidBody`.
//
// **`Resolved` is derived and everything else is authored.** One pass writes
// it, parent before child, and the draw pass and the hit test read it with a
// query rather than walking the tree again. A second copy of an absolute
// rectangle anywhere else in the engine is the stale-cache bug `scene::Bounds`
// refuses in as many words.
//
// ## Text and image names are `core::Name`, and that has a cost
//
// `Label::Text`, `Picture::Image` and `Entry::PlaceholderText` intern. That is
// what makes them `PropertyType::Name` — saved into a game file as text, sent
// over a wire as text, readable and writable from both bindings, and shown in
// the properties panel — with no new property type and no new wire form, which
// is exactly what `ecs::InstanceName` already trades for the same reasons.
//
// The cost is real and is stated rather than discovered: **text that changes
// every frame interns a new string every frame, and `core::Name` never
// releases one.** A score counter written as `label.Text = tostring(score)` is
// an unbounded growth in the process-wide registry, plus its mutex on every
// write. Filed as `D00020`; the fix is a string property type with its own
// storage, and it is a change to `ecs` rather than to this module.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/Enums.hpp>

#include <cstdint>

namespace engine::gui {

	// Where an element sits and how big it is, as authored.
	//
	// On every `GuiObject`. This is the whole input to the layout pass except
	// for the modifiers, and it is deliberately one component rather than four:
	// the pass reads all of it for every node it visits, so splitting it would
	// buy nothing and cost a second column lookup per node.
	//
	// @since v0.8
	struct Element {
		// Where the element's anchor point sits inside its parent.
		core::UDim2 Position;

		// How big the element is, against its parent's absolute size.
		core::UDim2 Size{0.0f, 100.0f, 0.0f, 100.0f};

		// Which point of the element `Position` places, as a fraction of its
		// own size. `(0,0)` is the top-left corner and `(0.5,0.5)` the centre.
		core::Vector2 AnchorPoint;

		// Clockwise rotation about the element's centre, in degrees.
		//
		// **Does not affect layout.** A rotated element occupies the same
		// rectangle as far as its parent and its siblings are concerned, which
		// is Roblox's behaviour and the only one that keeps layout a single
		// pass: a rotated child whose bounding box fed back into a list layout
		// would need the layout to run twice to settle.
		float Rotation = 0.0f;

		// Draw order among siblings. Higher draws later, so on top.
		int32_t ZIndex = 1;

		// The key a layout sorts children by, when its `SortOrder` says so.
		int32_t LayoutOrder = 0;

		// Whether this element and everything under it is drawn.
		//
		// **A branch and not a filter**, which is the opposite of
		// `scene::Visual::Visible`, and the difference is real: hiding a
		// container in a UI is expected to hide its contents, where hiding a
		// part is expected to leave its children alone. The compile stops
		// descending here, so an invisible subtree costs one test rather than
		// one test per node.
		bool Visible = true;

		// Whether children are clipped to this element's rectangle.
		bool ClipsDescendants = false;

		// Whether this element takes input rather than passing it through.
		//
		// A `GuiButton` behaves as though this were set whatever it holds,
		// which is Roblox's rule. On a `Frame` it is what turns a decorative
		// panel into one that swallows the click underneath it.
		bool Active = false;

		// Whether gamepad selection may land on this element.
		bool Selectable = false;

		// Which of the parent's axes a scale resolves against.
		SizeConstraint Constraint = SizeConstraint::RelativeXY;

		// Which axes grow to fit the content.
		AutomaticSize Automatic = AutomaticSize::None;
	};

	// The box an element draws for itself.
	//
	// Separate from `Element` because a `TextLabel` with a transparent
	// background still lays out, and because the draw pass reads this and never
	// reads a `UDim2`.
	//
	// @since v0.8
	struct Background {
		// The fill colour.
		core::Color3 Color{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible. Roblox's sense, kept because a
		// migrating script already contains it.
		float Transparency = 0.0f;

		// The outline colour.
		core::Color3 BorderColor{0.105f, 0.164f, 0.207f};

		// How thick the outline is, in pixels. Zero draws none.
		int32_t BorderSizePixel = 1;

		// Whether the outline grows outwards, inwards, or straddles the edge.
		BorderMode Border = BorderMode::Outline;
	};

	// The text an element shows.
	//
	// @since v0.8
	struct Label {
		// The string. Interned — see the note at the top of this file.
		core::Name Text;

		// The glyph colour.
		core::Color3 Color{0.105f, 0.164f, 0.207f};

		// 0 is opaque and 1 is invisible.
		float Transparency = 0.0f;

		// The em size in pixels, before `Scaled` is applied.
		int32_t Size = 14;

		// Which face to draw with, by role.
		FontFace Font = FontFace::Regular;

		// Horizontal placement inside the element.
		TextXAlignment XAlignment = TextXAlignment::Center;

		// Vertical placement inside the element.
		TextYAlignment YAlignment = TextYAlignment::Center;

		// Whether long text breaks onto further lines.
		bool Wrapped = false;

		// Whether the size shrinks until the text fits.
		//
		// **Overrides `Size` rather than clamping it**, which is Roblox's
		// behaviour: with this set, `Size` is the maximum and the drawn size is
		// whatever fits. A script reading `TextSize` back still sees what it
		// wrote; `Resolved::TextSize` is where the drawn number lands.
		bool Scaled = false;

		// What happens to text that still does not fit.
		TextTruncate Truncate = TextTruncate::None;

		// The outline colour drawn behind the glyphs.
		core::Color3 StrokeColor{0.0f, 0.0f, 0.0f};

		// 1 draws no stroke, which is Roblox's default and its sense.
		float StrokeTransparency = 1.0f;

		// Line spacing as a multiple of the em size.
		float LineHeight = 1.0f;
	};

	// The image an element shows.
	//
	// @since v0.8
	struct Picture {
		// The content name. Interned — see the note at the top of this file.
		core::Name Image;

		// Multiplied into the sampled colour. White leaves it alone.
		core::Color3 Color{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible.
		float Transparency = 0.0f;

		// How the image fills the rectangle.
		ScaleType Scale = ScaleType::Stretch;

		// The nine-slice centre, in image pixels. Only read for `Slice`.
		core::Rect SliceCenter;

		// How much the four corners of a nine-slice are magnified.
		float SliceScale = 1.0f;

		// The tile's size, for `Tile`.
		core::UDim2 TileSize{1.0f, 0.0f, 1.0f, 0.0f};

		// The top-left of the sub-rectangle to sample, in image pixels.
		core::Vector2 RectOffset;

		// The size of that sub-rectangle. Zero means the whole image.
		core::Vector2 RectSize;
	};

	// What makes a button a button.
	//
	// @since v0.8
	struct Button {
		// Whether the background lightens on hover and darkens on press.
		//
		// **Applied in the compile and not stored back**, so the authored
		// colour is what a script reads however the pointer is behaving. A
		// hover that wrote `Background::Color` would make the property's value
		// depend on where the mouse is, which is a script bug nobody could see.
		bool AutoButtonColor = true;

		// Whether this button swallows input to everything behind it.
		bool Modal = false;

		// Whether gamepad selection is currently on this button.
		bool Selected = false;
	};

	// What makes a scrolling frame scroll.
	//
	// @since v0.8
	struct Scrolling {
		// How big the scrollable area is, against the frame's absolute size.
		core::UDim2 CanvasSize{0.0f, 0.0f, 2.0f, 0.0f};

		// How far the canvas is scrolled, in pixels.
		core::Vector2 CanvasPosition;

		// How wide the bars are, in pixels. Zero hides them.
		int32_t BarThickness = 12;

		// The bar's colour.
		core::Color3 BarColor{0.098f, 0.098f, 0.098f};

		// 0 is opaque and 1 is invisible.
		float BarTransparency = 0.0f;

		// Whether input may scroll it. Independent of whether bars are drawn.
		bool Enabled = true;

		// Which axes may be scrolled.
		ScrollingDirection Direction = ScrollingDirection::Y;

		// Which axes grow the canvas to fit the content.
		AutomaticSize AutomaticCanvas = AutomaticSize::None;
	};

	// What makes a text box editable.
	//
	// @since v0.8
	struct Entry {
		// Shown when the text is empty. Interned — see the top of this file.
		core::Name PlaceholderText;

		// The placeholder's colour.
		core::Color3 PlaceholderColor{0.69f, 0.69f, 0.69f};

		// Whether focusing empties it.
		bool ClearTextOnFocus = true;

		// Whether Return inserts a line break instead of releasing focus.
		bool MultiLine = false;

		// Whether a person may type into it at all.
		bool TextEditable = true;

		// The caret's index into the text, or -1 when unfocused.
		int32_t CursorPosition = -1;

		// Where a selection started, or -1 when there is none.
		int32_t SelectionStart = -1;
	};

	// A root that hands its subtree somewhere to be drawn.
	//
	// On every `LayerCollector`. What differs between a `ScreenGui`, a
	// `SurfaceGui` and a `BillboardGui` is *where* the canvas is, which is what
	// `Canvas`, `Surface` and `Billboard` add — the fields here are the ones all
	// three share.
	//
	// @since v0.8
	struct Layer {
		// Whether this collector and its subtree are drawn at all.
		bool Enabled = true;

		// Which collector draws on top. Higher draws later.
		int32_t DisplayOrder = 0;

		// Whether `ZIndex` is compared across the collector or among siblings.
		ZIndexBehavior Behavior = ZIndexBehavior::Sibling;

		// Whether the tree is rebuilt when the player respawns.
		bool ResetOnSpawn = true;

		// Whether the canvas covers the top bar's strip as well.
		bool IgnoreGuiInset = false;
	};

	// The screen-sized canvas a `ScreenGui` collects onto.
	//
	// Holds the resolved rectangle rather than any authored field, because a
	// screen gui has none — its canvas is the viewport. It exists so the draw
	// pass and the hit test can read the canvas a node belongs to without
	// learning which of the three kinds of collector it was.
	//
	// @since v0.8
	struct Canvas {
		// The rectangle this collector's roots lay out inside, in pixels.
		core::Rect Area;
	};

	// A collector projected onto a face of a part.
	//
	// @since v0.8
	struct Surface {
		// The part this is drawn on, or the parent when unset.
		ecs::Entity Adornee;

		// Which face of that part.
		Face On = Face::Front;

		// How the canvas's pixel size is decided.
		SurfaceSizingMode Sizing = SurfaceSizingMode::FixedSize;

		// Pixels per stud, when `Sizing` says so.
		float PixelsPerStud = 50.0f;

		// The canvas size in pixels, when `Sizing` says `FixedSize`.
		core::Vector2 CanvasSize{800.0f, 600.0f};

		// Whether it draws over geometry in front of it.
		bool AlwaysOnTop = false;

		// How much scene lighting tints it. 0 is fullbright.
		float LightInfluence = 0.0f;

		// A multiplier on the fullbright part of the result.
		float Brightness = 1.0f;
	};

	// A collector that faces the camera at a point in the world.
	//
	// @since v0.8
	struct Billboard {
		// The part this hangs off, or the parent when unset.
		ecs::Entity Adornee;

		// How big the billboard is, in studs or pixels per its `Element`.
		core::UDim2 Size{0.0f, 200.0f, 0.0f, 50.0f};

		// An offset in the camera's own axes, in studs.
		core::Vector3 StudsOffset;

		// An offset in world axes, in studs.
		core::Vector3 StudsOffsetWorldSpace;

		// An offset in multiples of the adornee's size.
		core::Vector3 ExtentsOffset;

		// Whether it draws over geometry in front of it.
		bool AlwaysOnTop = false;

		// How much scene lighting tints it. 0 is fullbright.
		float LightInfluence = 0.0f;

		// Beyond this many studs it is not drawn. Zero means no limit.
		float MaxDistance = 0.0f;
	};

	// What a `CanvasGroup` composites its subtree with.
	//
	// @since v0.8
	struct Group {
		// Multiplied into every descendant's colour.
		core::Color3 Color{1.0f, 1.0f, 1.0f};

		// Applied to the composited subtree as a whole, which is the entire
		// point of the class: fading a group fades it once rather than fading
		// each child and showing the overlaps.
		float Transparency = 0.0f;
	};

	// What a `ViewportFrame` renders into itself.
	//
	// @since v0.8
	struct Viewport {
		// The camera to render from, or unset for none.
		ecs::Entity CurrentCamera;

		// The ambient term applied inside the frame.
		core::Color3 Ambient{0.78f, 0.78f, 0.78f};

		// The colour of the frame's own directional light.
		core::Color3 LightColor{0.54f, 0.54f, 0.54f};

		// That light's direction, in the frame's own space.
		core::Vector3 LightDirection{-1.0f, -1.0f, -1.0f};

		// Multiplied into the rendered image.
		core::Color3 Color{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible.
		float Transparency = 0.0f;
	};

	// --- the modifiers ------------------------------------------------------
	//
	// Each is a component on a child instance rather than a field on the
	// parent, which is Roblox's shape and is the right one here for the ECS's
	// own reason: most elements have no padding and no layout, and a field per
	// modifier on `Element` would be that memory on every node in every UI.

	// Space held back inside an element before its children are placed.
	//
	// @since v0.8
	struct Padding {
		core::UDim Top;
		core::UDim Bottom;
		core::UDim Left;
		core::UDim Right;
	};

	// Stacks the parent's children along one axis.
	//
	// @since v0.8
	struct ListLayout {
		// Which way the children stack.
		FillDirection Direction = FillDirection::Vertical;

		// Space between one child and the next.
		core::UDim Padding;

		// Where the stack sits along the horizontal axis.
		HorizontalAlignment Horizontal = HorizontalAlignment::Left;

		// Where the stack sits along the vertical axis.
		VerticalAlignment Vertical = VerticalAlignment::Top;

		// What order the children are visited in.
		SortOrder Order = SortOrder::LayoutOrder;
	};

	// Places the parent's children on a grid.
	//
	// @since v0.8
	struct GridLayout {
		// How big each cell is, against the parent's absolute size.
		core::UDim2 CellSize{0.0f, 100.0f, 0.0f, 100.0f};

		// Space between cells, on both axes.
		core::UDim2 CellPadding{0.0f, 5.0f, 0.0f, 5.0f};

		// Which way a row or column is filled.
		FillDirection Direction = FillDirection::Horizontal;

		// How many cells before wrapping. Zero fits as many as the parent
		// holds, which is Roblox's meaning of its own default.
		int32_t MaxCells = 0;

		// Which corner the fill starts from.
		StartCorner Corner = StartCorner::TopLeft;

		// Where the grid sits along the horizontal axis.
		HorizontalAlignment Horizontal = HorizontalAlignment::Left;

		// Where the grid sits along the vertical axis.
		VerticalAlignment Vertical = VerticalAlignment::Top;

		// What order the children are visited in.
		SortOrder Order = SortOrder::LayoutOrder;
	};

	// Forces the parent's resolved size to a ratio.
	//
	// @since v0.8
	struct AspectRatio {
		// Width divided by height.
		float Ratio = 1.0f;

		// Whether the ratio fits inside the resolved size or scales with the
		// parent's.
		AspectType Type = AspectType::FitWithinMaxSize;

		// Which axis the other is derived from.
		DominantAxis Dominant = DominantAxis::Width;
	};

	// Clamps the parent's resolved size, in pixels.
	//
	// @since v0.8
	struct SizeLimits {
		core::Vector2 Min{0.0f, 0.0f};

		// The default is the largest a UI could sensibly be rather than
		// infinity, so a clamp against an unset maximum is arithmetic on a real
		// number instead of a special case every reader has to remember.
		core::Vector2 Max{1.0e6f, 1.0e6f};
	};

	// Clamps the parent's resolved text size, in pixels.
	//
	// @since v0.8
	struct TextSizeLimits {
		int32_t Min = 1;
		int32_t Max = 100;
	};

	// Rounds the parent's corners.
	//
	// @since v0.8
	struct Corner {
		// Resolved against the parent's *smaller* axis, which is what stops a
		// scale of 0.5 producing an ellipse on a wide element.
		core::UDim Radius{0.0f, 8.0f};
	};

	// Draws an outline around the parent, outside its own border.
	//
	// @since v0.8
	struct Stroke {
		core::Color3 Color{0.0f, 0.0f, 0.0f};
		float Thickness = 1.0f;
		float Transparency = 0.0f;
	};

	// Multiplies the parent's resolved size and text size.
	//
	// @since v0.8
	struct Scale {
		float Factor = 1.0f;
	};

	// --- what the layout pass produces --------------------------------------

	// Where an element actually ended up.
	//
	// **Derived, and the only component here that is.** One pass writes it,
	// parent before child; the draw pass and the hit test read it with a query.
	// Nothing else in the engine may keep a second copy — `scene::Bounds` gives
	// the argument, and it is the same argument.
	//
	// A node that is not reached by the pass keeps whatever it last held and is
	// marked not `Rendered`, rather than being zeroed. Zeroing would make an
	// element that scrolled out of view indistinguishable from one that has
	// never been laid out, and the hit test would then have to guess.
	//
	// @since v0.8
	struct Resolved {
		// The top-left corner, in canvas pixels.
		core::Vector2 AbsolutePosition;

		// The extent, in canvas pixels.
		core::Vector2 AbsoluteSize;

		// Total clockwise rotation, in degrees, including every ancestor's.
		float AbsoluteRotation = 0.0f;

		// The rectangle this element is clipped to, in canvas pixels.
		//
		// The intersection of every clipping ancestor's rectangle with the
		// canvas. Inherited rather than recomputed per draw, which is what
		// makes clipping cost one intersection per node instead of a walk.
		core::Rect Clip;

		// The text size actually drawn, after `Label::Scaled` and any
		// `TextSizeLimits`. Equal to `Label::Size` when neither applies.
		int32_t TextSize = 0;

		// How deep in the tree, counted from the collector. The tiebreak that
		// makes paint order total.
		int32_t Depth = 0;

		// The position in the compile's paint order. Assigned by the compile
		// and read by nothing else, but stored rather than local so that a
		// panel or a test can ask why one element covered another.
		int32_t Order = 0;

		// Whether this element descends from an enabled collector and every
		// ancestor between was visible.
		//
		// **Derived by the pass and not maintained at the write**, exactly as
		// `scene::Visual`'s rendered flag is: ancestry is not local, so hooking
		// every parenting path is where one gets missed and an element draws
		// after being detached.
		bool Rendered = false;
	};
}
