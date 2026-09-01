#pragma once

// What a 2D thing in a game *is* - the component set both halves share.
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
// ## Text is owned; an image name is interned
//
// **The split is what a value *is*, not what it is made of.** `Picture::Image`
// is an asset id - one of the bounded set of things a game shipped - so it is a
// `core::Name`, interned once and compared as an integer, exactly as
// `Material` and a class name are.
//
// `Label::Text` and `Entry::PlaceholderText` are `std::string`, and that is
// `D00020` closed. They were `core::Name` too, and the cost was stated in this
// comment for a version before it was paid: **`core::Name` never releases**, so
// `label.Text = tostring(score)` at sixty hertz interned a new string every
// frame, forever, and took the process-wide registry's mutex inside the frame
// loop to do it. A score counter is the first thing anybody writes.
//
// What it costs instead: `Label` and `Entry` are no longer trivially copyable,
// so both carry a written serialiser - which both already did, because a
// `core::Name`'s id is process-local and could never have been memcpy'd to a
// file either. The storage change is therefore paid entirely in `ecs::Column`'s
// non-trivial path, which has existed since v0.2 and had no user until now.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/Sequence.hpp>
#include <engine/core/types/TweenInfo.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/gui/Enums.hpp>

#include <cstdint>
#include <string>

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

		// Whether the element responds to a pointer at all.
		//
		// **Not `Active` with a second name.** `Active` decides whether a click
		// *stops* here or passes through to what is behind; this decides whether
		// the element reacts, and a `GuiButton` is `Active` by class whatever it
		// says. Clearing this on a button is how a game greys one out: it still
		// swallows the click, it just does nothing with it and stops lighting up
		// under the pointer.
		//
		// @since v0.18
		bool Interactable = true;

		// Which of the parent's axes a scale resolves against.
		SizeConstraint Constraint = SizeConstraint::RelativeXY;

		// Which axes grow to fit the content.
		AutomaticSize Automatic = AutomaticSize::None;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[1] = {};
	};

	// Where a gamepad's selection goes from this element, and what marks it.
	//
	// **A component of its own rather than five more fields on `Element`.** The
	// layout pass reads `Element` for every node in the tree every frame and
	// would load forty bytes of handles it never looks at; this is read only when
	// the selection moves, which is a keypress rather than a frame. That is
	// `ecs/AGENTS.md`'s split-by-what-is-iterated rule, and it is the same one
	// that keeps `Label` off a `Frame`.
	//
	// **Every field is optional and null is the useful default**, which is what
	// lets the component sit on every `GuiObject` and cost nothing to ignore:
	// `SelectNext` scores by direction unless one of these names an answer.
	//
	// @since v0.18
	struct Selection {
		// Where the selection goes from here, per direction. Null lets
		// `SelectNext` work it out by geometry.
		//@{
		ecs::Entity NextUp;
		ecs::Entity NextDown;
		ecs::Entity NextLeft;
		ecs::Entity NextRight;
		//@}

		// A `GuiObject` drawn over this element while it is selected, or null
		// for the default highlight.
		//
		// **Drawn over rather than cloned into the tree**, which is the one place
		// this differs from Roblox and it is deliberate: a clone would be an
		// instance nothing authored appearing in the explorer, in `GetChildren`
		// and in a save file. `Compile.cpp` emits the referenced object's own
		// fill and image at the selected element's rectangle instead, which is
		// what a highlight is for and needs no second tree.
		ecs::Entity ImageObject;

		// Which element `SelectNext` seeds on when nothing is selected. Lower
		// goes first, which is Roblox's sense.
		int32_t Order = 0;

		// Explicit padding, for the reason every other `Reserved` gives. The
		// handles align the whole component to eight.
		uint8_t Reserved[4] = {};
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

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// The text an element shows.
	//
	// @since v0.8
	struct Label {
		// The string. Owned rather than interned - see the note at the top of
		// this file, and `ecs::PropertyType::String`.
		std::string Text;

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

		// How many characters of the string are shown, or -1 for all of it.
		//
		// **Characters and not bytes**, which is `Entry::CursorPosition`'s
		// distinction and matters for the same reason: a typewriter effect
		// counting bytes reveals half of an accented letter and draws a
		// replacement glyph. `src/Utf8.hpp` is the one place that crosses
		// between the two.
		//
		// **Roblox calls these graphemes and this engine counts codepoints**,
		// which differ for a letter written as a base plus a combining accent
		// and for an emoji built from several. Naming the property Roblox's way
		// and stating the difference here is the honest trade: closing it needs a
		// grapheme-breaking table, which is a `core` dependency this module does
		// not have.
		//
		// @since v0.18
		int32_t MaxVisible = -1;

		// Whether the string is read as markup rather than as literal text.
		//
		// **The parse produces spans and never a second command.** A run is one
		// `DrawCommand` with a list of styled byte ranges over it, because the
		// backend is the only thing that can measure a glyph - laying spans out
		// here would put compile-time estimates next to run-time metrics and the
		// two would disagree about where the second word starts. `DrawSpan`
		// carries the argument at the other end.
		//
		// @since v0.18
		bool Rich = false;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// The image an element shows.
	//
	// @since v0.8
	struct Picture {
		// The content name. Interned - see the note at the top of this file.
		core::Name Image;

		// Multiplied into the sampled colour. White leaves it alone.
		core::Color3 Color{1.0f, 1.0f, 1.0f};

		// 0 is opaque and 1 is invisible.
		float Transparency = 0.0f;

		// How the image fills the rectangle.
		ScaleType Scale = ScaleType::Stretch;

		// The nine-slice centre, in image pixels. Only read for `Slice`.
		core::Rect SliceCenter = {};

		// How much the four corners of a nine-slice are magnified.
		float SliceScale = 1.0f;

		// The tile's size, for `Tile`.
		core::UDim2 TileSize{1.0f, 0.0f, 1.0f, 0.0f};

		// The top-left of the sub-rectangle to sample, in image pixels.
		core::Vector2 RectOffset = {};

		// The size of that sub-rectangle. Zero means the whole image.
		core::Vector2 RectSize = {};

		// Which fragment shader samples and colours this image, or invalid
		// for the interface pass's own.
		//
		// **`scene::SurfaceAppearance::Shader`'s exact shape, one indirection
		// flatter.** A part's shader is authored on a `Material` child because
		// one mesh may show several materials; one `ImageLabel` shows exactly
		// one image, so there is nothing a second instance would let an
		// author say that this field cannot. `render::ShaderLibrary` resolves
		// it the same two ways either shows it: a `ShaderScript` in the world
		// under this name, compiled while the engine runs, else a built-in
		// this engine ships.
		//
		// **A name and not a handle**, rule 4: it survives a save file and a
		// wire, and a headless host can hold and replicate it without a
		// device.
		//
		// @since v0.18
		core::Name Shader;

		// What an `ImageButton` shows instead of `Image` under the pointer and
		// while it is held. Unset leaves `Image` showing.
		//
		// **On `Picture` rather than on `Button`, because they are images and
		// `Picture` is where an image lives.** A `TextButton` never has one and
		// pays four bytes each for the pair, which is the same trade `Element`
		// makes for `Selectable` on a `Frame` - and the alternative is a second
		// component that exists to hold two names.
		//
		// **Swapped in the compile and not stored back**, exactly as
		// `Button::AutoButtonColor`'s shift is: `Image` reads what the author
		// wrote however the pointer is behaving.
		//
		// @since v0.18
		//@{
		core::Name HoverImage;
		core::Name PressedImage;
		//@}

		// How the texels are filtered when the image is not drawn at its own
		// size.
		//
		// @since v0.18
		ResampleMode Resample = ResampleMode::Default;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// What makes a button a button.
	//
	// @since v0.8
	struct Button {
		// Whether the background shifts under the pointer, further on press
		// than on hover.
		//
		// **Away from whichever end the fill is already at, not always
		// lighter**, because the default fill is white and lightening white is
		// no shift at all. `Compile.cpp`'s `ShiftDirection` is where that is
		// decided and says why.
		//
		// **Applied in the compile and not stored back**, so the authored
		// colour is what a script reads however the pointer is behaving. A
		// hover that wrote `Background::Color` would make the property's value
		// depend on where the mouse is, which is a script bug nobody could see.
		bool AutoButtonColor = true;

		// Reserved for mouse-capture policy. It is not exposed as a property until
		// the input router has a modal route to apply it to.
		bool Modal = false;

		// Reserved for a per-button selection cache. `GuiService.SelectedObject`
		// owns selection today, so exposing a second writable answer would let the
		// two disagree.
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

		// The bar's colour.
		core::Color3 BarColor{0.098f, 0.098f, 0.098f};

		// The three images a bar is drawn from - its two caps and the length
		// between them, nine-sliced along the bar's own axis.
		//
		// **Names and not handles**, rule 4, and unset means the bar is drawn as
		// a plain rounded rectangle. That is the fallback rather than a missing-
		// image marker because a scroll bar with no art is a perfectly ordinary
		// thing to ship and an author who set none did not make a mistake.
		//
		// @since v0.18
		//@{
		core::Name TopImage;
		core::Name MidImage;
		core::Name BottomImage;
		//@}

		// How wide the bars are, in pixels. Zero hides them.
		int32_t BarThickness = 12;

		// 0 is opaque and 1 is invisible.
		float BarTransparency = 0.0f;

		// Whether a wheel or a drag may move the canvas.
		//
		// **`CanvasPosition` is still writable when this is false**, which is
		// Roblox's rule and the useful one: a script driving a carousel wants to
		// place the canvas itself and wants a person's wheel to leave it alone.
		bool Enabled = true;

		// Which axes may be scrolled.
		ScrollingDirection Direction = ScrollingDirection::Y;

		// Which axes grow the canvas to fit what is inside the frame.
		//
		// **The content's extent replaces that axis of `CanvasSize` rather than
		// adding to it**, which is Roblox's behaviour: a list that grows a row
		// scrolls one row further, and a `CanvasSize` an author left at its
		// default does not double the answer.
		AutomaticSize AutomaticCanvas = AutomaticSize::None;

		// Whether a drag may pull the canvas past its end.
		//
		// **Animated since v0.18, and this comment said otherwise until v0.19.**
		// It read "declared, pinned and not yet animated", on the argument that
		// a spring needs a clock and `gui` is L7 with no notion of a frame time.
		// `ScrollMotion` is that clock: `gui::Router` reads this field when a
		// drag runs past the end, and the spring in `gui::Layout` returns the
		// canvas after release. All three `ElasticBehavior` values have a case
		// in `engine.gui.input`.
		ElasticBehavior Elastic = ElasticBehavior::WhenScrollable;

		// Whether each bar eats into the room the content gets.
		//@{
		ScrollBarInset HorizontalInset = ScrollBarInset::None;
		ScrollBarInset VerticalInset = ScrollBarInset::None;
		//@}

		// Which side the vertical bar sits on.
		BarPosition VerticalBar = BarPosition::Right;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[1] = {};
	};

	// What the layout worked out about a scrolling frame.
	//
	// **Derived, like `Resolved`, and written by the same pass.** Three
	// consumers need these numbers and none of them may recompute them: a
	// script reads `AbsoluteCanvasSize`, the compile draws the bars, and the
	// router hit-tests a drag against the thumb. Three copies of the same
	// arithmetic is the stale-cache bug `Resolved` exists to refuse, one class
	// along - and the specific failure here is a thumb you can see and cannot
	// grab.
	//
	// @since v0.18
	struct ScrollState {
		// The scrollable extent in pixels, after `AutomaticCanvas`.
		core::Vector2 CanvasSize;

		// The visible extent in pixels, after any bar inset.
		core::Vector2 WindowSize;

		// Where each thumb is, in canvas pixels. Empty when that bar is not
		// shown, which is what makes "is there a bar here" one test rather than
		// a repeat of the decision that produced it.
		//@{
		core::Rect VerticalThumb;
		core::Rect HorizontalThumb;
		//@}
	};

	// Where a `ScrollingFrame` has been pulled past its end, and how it is
	// getting back.
	//
	// **`ElasticBehavior` had two blockers and the clock was the smaller one.**
	// The other is that nothing dragged a canvas at all: until v0.17 the only
	// caller of the scroll mover was the wheel, and a wheel does not overscroll
	// in Roblox either. So a pull needs something to pull it, which is
	// `Router::BeginCanvasDrag` and the two calls after it.
	//
	// The spring is a **function of elapsed time** rather than a stepped
	// integration, which is `render::FlipbookFrameAt`'s shape and buys the same
	// thing: a suite states where the canvas is a tenth of a second after the
	// release rather than stepping a hundred frames to find out. It also makes
	// the recovery independent of frame rate, which a per-frame multiply is not.
	//
	// Local, never replicated - see `PageMotion` for why a clock reading must
	// not cross.
	//
	// @since v0.17
	struct ScrollMotion {
		// When the pull was let go of, or negative while nothing is springing.
		//
		// **First, because it is the widest member.** Everything below is four
		// or one byte, so putting the `double` anywhere else opens a hole that
		// `ecs::AuditComponents` refuses - a component serialised as its object
		// representation carries whatever is in that hole into every save.
		double ReleasedAt = -1.0;

		// How far past the end the canvas is being held, in pixels, signed.
		// Negative is past the near end and positive past the far end.
		core::Vector2 Pull;

		// The pull that was let go of, which the spring decays from.
		core::Vector2 Released;

		// Where the pointer was when the drag last moved the canvas, so the
		// next move is a delta rather than an absolute.
		core::Vector2 LastPoint;

		// How far past the end the canvas is drawn right now, in pixels.
		// Resolved once per layout from the two above, so `ContentArea` reads a
		// number and never a clock.
		//
		// Zeroed when the spring has settled under a pixel, which is what stops
		// the signature moving for a frame nobody can see.
		core::Vector2 Overshoot;

		// Whether a drag is holding the canvas right now.
		bool Held = false;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[7] = {};
	};

	// What makes a text box editable.
	//
	// @since v0.8
	struct Entry {
		// Shown when the text is empty. Owned, like `Label::Text` - a
		// placeholder is authored rather than chosen from a set, and having
		// the two strings a text box holds be two different types would be a
		// distinction with nothing behind it.
		std::string PlaceholderText;

		// The placeholder's colour.
		core::Color3 PlaceholderColor{0.69f, 0.69f, 0.69f};

		// Whether focusing empties it.
		bool ClearTextOnFocus = true;

		// Whether Return inserts a line break instead of releasing focus.
		bool MultiLine = false;

		// Whether a person may type into it at all.
		bool TextEditable = true;

		// The caret's position in the text, or -1 when unfocused.
		//
		// **Roblox's number: one-based, and counted in characters rather than
		// in bytes.** `1` is before the first character and `n + 1` is after the
		// last, so an empty box that has just taken focus reads `1`. The
		// distinction is not academic - `Label::Text` is UTF-8, and a caret set
		// from `Text.size()` sits past the end of anything typed in a language
		// with accents in it. `gui::Focus` counts the characters.
		int32_t CursorPosition = -1;

		// Where a selection started, or -1 when there is none.
		int32_t SelectionStart = -1;
	};

	// A root that hands its subtree somewhere to be drawn.
	//
	// On every `LayerCollector`. What differs between a `ScreenGui`, a
	// `SurfaceGui` and a `BillboardGui` is *where* the canvas is, which is what
	// `Canvas`, `Surface` and `Billboard` add - the fields here are the ones all
	// three share.
	//
	// @since v0.8
	// Widest first, which is the ordering `TypeDescriptor`'s warning asks for
	// rather than the reading order the fields were written in. A `bool` ahead
	// of the `int32_t` cost three bytes nothing wrote, and they went into every
	// save.
	struct Layer {
		// Which collector draws on top. Higher draws later.
		int32_t DisplayOrder = 0;

		// Whether this collector and its subtree are drawn at all.
		bool Enabled = true;

		// Whether `ZIndex` is compared across the collector or among siblings.
		ZIndexBehavior Behavior = ZIndexBehavior::Sibling;

		// Whether the tree is rebuilt when the player respawns.
		bool ResetOnSpawn = true;

		// Whether the canvas covers the top bar's strip as well.
		bool IgnoreGuiInset = false;
	};

	// The canvas rectangle a collector's roots lay out inside.
	//
	// **Every collector, not only a `ScreenGui`, which is what this said until
	// v0.19.** `gui::Layout` writes one for a `SurfaceGui` and a `BillboardGui`
	// too, and its one reader in `gui::Compile` exists *specifically* for a
	// spatial collector with `ClipsDescendants` off - a case a screen gui can
	// never be in, because a screen gui always clips.
	//
	// Holds the resolved rectangle rather than any authored field, because a
	// collector authors none - a screen gui's canvas is the viewport and a
	// spatial one's is fitted by whoever holds a camera. It exists so the draw
	// pass and the hit test can read the canvas a node belongs to without
	// learning which of the three kinds of collector it was.
	//
	// **Local to the machine looking**, therefore, and
	// `replication::LocalToTheClient` names it. It did not until v0.19, so the
	// authority's screen rectangle crossed to every client and was overwritten
	// by that client's next layout pass - `gui.Resolved`, written three lines
	// later off the same rectangle, had been excluded from the start.
	//
	// @since v0.8
	struct Canvas {
		// The rectangle this collector's roots lay out inside, in pixels.
		core::Rect Area;
	};

	// The pixel canvas a spatial collector was resolved to.
	//
	// **The seam `D00022` named, and it is a slot rather than an answer.** A
	// `SurfaceGui` sized in pixels-per-stud needs the stud extent of the face it
	// is on, which is `scene::Bounds`; a `BillboardGui`'s scale is against the
	// screen it is projected onto, which is a fact about a camera and a
	// viewport. This module is L7 `shared` and links neither, so it declares
	// where the answer goes and whoever holds both operands writes it -
	// `render::ResolveSpatialCanvases` is that writer today.
	//
	// **Derived, like `Canvas` and `Resolved`.** Nothing authors it, nothing
	// saves a meaningful value into it and nothing replicates it: a host
	// recomputes it every frame from the camera it is drawing with, and two
	// hosts with different viewports are *supposed* to disagree.
	//
	// Absent means "nobody resolved one", and `CanvasFor` then falls back to the
	// authored pixel size. A drawable fixed-size surface still receives this
	// component because the renderer needs its world plane as well as its pixels.
	//
	// @since v0.8
	enum class SpatialCanvasKind : uint8_t {
		Surface,
		Billboard,
	};

	// One collector's resolved placement in the world, ready for the renderer.
	//
	// **Resolved once per frame rather than read per draw.** Both kinds carry
	// every field: the alternative is two components, and a reader that has to
	// ask which one it is before it can read a position.
	//
	// @since v0.8
	struct SpatialCanvas {
		// The canvas size in pixels.
		core::Vector2 Size;

		// How the canvas is placed. A surface uses `Origin` and both full-span
		// axes directly. A billboard uses `Origin` as its anchor and `WorldSize`
		// against the camera that draws it.
		//@{
		core::Vector3 Origin;
		core::Vector3 AxisX;
		core::Vector3 AxisY;
		core::Vector2 WorldSize;
		core::Vector2 BillboardStuds;
		core::Vector2 BillboardPixels;
		core::Vector3 Normal;
		//@}

		// How the world lights it, and how far away it stops being drawn.
		//
		// `LightInfluence` blends between unlit and lit, `Brightness` scales the
		// result, and `MaxDistance` of zero means no limit rather than "never
		// draw" - which is the reading a default-constructed component needs.
		//@{
		float LightInfluence = 0.0f;
		float Brightness = 1.0f;
		float MaxDistance = 0.0f;
		//@}

		// How far the camera that resolved this was from the collector, in studs.
		//
		// **Both kinds carry it, and only one of them has a property for it.**
		// `BillboardGui.CurrentDistance` is the readable half - a script fading a
		// name tag by range wants the number the size was already computed from
		// rather than one it recomputes from a camera it has to find. A surface
		// writes it too because `MaxDistance` is decided against the same
		// measurement and a reader of one wants the other.
		//
		// @since v0.18
		float CurrentDistance = 0.0f;

		// Which of the two placements above the renderer should read.
		SpatialCanvasKind Kind = SpatialCanvasKind::Surface;

		// Whether it draws over the world rather than being depth-tested
		// against it, and whether it draws at all.
		//@{
		bool AlwaysOnTop = false;
		bool Visible = true;
		//@}

		// Whether a pointer projected onto the world may land on this canvas.
		//
		// **Resolved rather than read from the collector at the point of use**,
		// because the reader is `render::ResolveSpatialPointer`, which walks this
		// component and nothing else - asking each candidate which of the two
		// classes it is would be a class lookup per collector per pointer sample
		// for a fact the resolve already had.
		//
		// @since v0.18
		bool Interactive = false;

		// Padding, kept explicit so the component's layout is stated rather than
		// left to the compiler. `ecs`'s padding rule is what makes that matter:
		// an unnamed hole is bytes a hash and a wire format both have to be told
		// to skip.
		uint8_t Reserved[4] = {};
	};

	// A collector projected onto a face of a part.
	//
	// @since v0.8
	// Widest first, for `Layer`'s reason.
	struct Surface {
		// The part this is drawn on, or the parent when unset.
		ecs::Entity Adornee;

		// The canvas size in pixels, when `Sizing` says `FixedSize`.
		core::Vector2 CanvasSize{800.0f, 600.0f};

		// Pixels per stud, when `Sizing` says so.
		float PixelsPerStud = 50.0f;

		// How much scene lighting tints it. 0 is fullbright.
		float LightInfluence = 0.0f;

		// A multiplier on the fullbright part of the result.
		float Brightness = 1.0f;

		// How far off the face the canvas floats, in studs.
		//
		// **Zero still floats, and the amount is not authored.** A canvas exactly
		// on a face z-fights with it, so the resolve adds a hairline bias
		// whatever this holds; this is what an author adds on top to lift a sign
		// clear of a bumpy wall or to stack two surfaces on one face.
		//
		// @since v0.18
		float ZOffset = 0.0f;

		// Beyond this many studs from the camera it is not drawn. Zero means no
		// limit.
		//
		// **A thousand rather than zero**, which is Roblox's default and is the
		// one default in this struct chosen against the engine's usual "the
		// unset value is the inert one" rule. A sign legible at a thousand studs
		// is a sign smaller than a pixel, and the alternative default has every
		// surface in a world resolving and compiling forever.
		//
		// @since v0.18
		float MaxDistance = 1000.0f;

		// Which face of that part.
		Face On = Face::Front;

		// How the canvas's pixel size is decided.
		SurfaceSizingMode Sizing = SurfaceSizingMode::FixedSize;

		// Whether it draws over geometry in front of it.
		bool AlwaysOnTop = false;

		// Whether what its subtree draws is cut to the canvas.
		//
		// @since v0.18
		bool ClipsDescendants = true;

		// Whether a pointer in the world may land on it at all.
		//
		// **False by default, which is Roblox's and is the surprising half.** A
		// surface gui is a decal before it is a control panel, and an interactive
		// one intercepts every click that crosses its plane - including the ones
		// meant for the part behind it. Making that opt-in is what stops a
		// decorative sign swallowing a player's aim.
		//
		// @since v0.18
		bool Active = false;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[7] = {};
	};

	// A collector that faces the camera at a point in the world.
	//
	// @since v0.8
	struct Billboard {
		// The part this hangs off, or the parent when unset.
		ecs::Entity Adornee;

		// A viewer this one is hidden from, or null.
		//
		// **A `Player` instance, and the check is the drawing host's.** A
		// billboard is resolved once per world and drawn once per viewer, so the
		// only place "is this me" can be asked is where a viewer is known - which
		// is `client::Client` holding its own `Player`, not this module and not
		// `ResolveSpatialCanvases`. What is here is the authored half: which
		// player, by handle, replicated like every other field.
		//
		// @since v0.18
		ecs::Entity PlayerToHideFrom;

		// How big the billboard is, in studs or pixels per its `Element`.
		core::UDim2 Size{0.0f, 200.0f, 0.0f, 50.0f};

		// An offset in the camera's own axes, in studs.
		core::Vector3 StudsOffset;

		// An offset in world axes, in studs.
		core::Vector3 StudsOffsetWorldSpace;

		// An offset in multiples of the adornee's size, in the adornee's axes.
		core::Vector3 ExtentsOffset;

		// The same offset in world axes, so it does not turn with the adornee.
		//
		// **The pair is the point.** A name tag over a rotating part wants to
		// stay above it, which is this one; an arrow pinned to a part's own nose
		// wants to turn with it, which is the field above. Roblox carries both
		// for exactly that reason and they add rather than override.
		//
		// @since v0.18
		core::Vector3 ExtentsOffsetWorldSpace;

		// An anchor point, in multiples of the billboard's own size.
		//
		// **Roblox's numbering and not `AnchorPoint`'s**, which is worth stating
		// because the two look interchangeable and are not: this is signed and
		// centred, so `(0, 0)` centres the billboard on its anchor and
		// `(0.5, 0.5)` puts the anchor at the *bottom left* of it. A `GuiObject`'s
		// `AnchorPoint` is unsigned and measured from the top left.
		//
		// @since v0.18
		core::Vector2 SizeOffset;

		// How much scene lighting tints it. 0 is fullbright.
		float LightInfluence = 0.0f;

		// A multiplier on the fullbright part of the result.
		//
		// @since v0.18
		float Brightness = 1.0f;

		// Beyond this many studs it is not drawn. Zero means no limit.
		float MaxDistance = 0.0f;

		// How coarsely the distance the size is computed from is quantised, in
		// studs. Zero measures exactly.
		//
		// **What it buys is not performance.** A billboard scaled from an exact
		// distance changes size every frame a player walks, and text re-fitted
		// every frame shimmers; stepping the distance makes the size change in
		// visible jumps that hold still in between, which is the trade Roblox
		// offers and the reason the property is not a boolean.
		//
		// @since v0.18
		float DistanceStep = 0.0f;

		// Whether it draws over geometry in front of it.
		bool AlwaysOnTop = false;

		// Whether what its subtree draws is cut to the canvas.
		//
		// @since v0.18
		bool ClipsDescendants = true;

		// Whether a pointer in the world may land on it at all.
		//
		// @since v0.18
		bool Active = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[5] = {};
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

		// Explicit padding, for the reason every other `Reserved` gives.
		// `CurrentCamera` aligns the whole component to eight.
		uint8_t Reserved[4] = {};
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
		// One edge each, resolved against the parent's own size.
		//
		// **A `UDim` rather than a number**, so padding can be a fraction of the
		// parent - which is what lets one element look right at two sizes
		// without a script recomputing it.
		//@{
		core::UDim Top;
		core::UDim Bottom;
		core::UDim Left;
		core::UDim Right;
		//@}
	};

	// Stacks the parent's children along one axis.
	//
	// @since v0.8
	// Widest first, for `Layer`'s reason.
	struct ListLayout {
		// Space between one child and the next.
		core::UDim Padding;

		// Which way the children stack.
		FillDirection Direction = FillDirection::Vertical;

		// Where the stack sits along the horizontal axis.
		HorizontalAlignment Horizontal = HorizontalAlignment::Left;

		// Where the stack sits along the vertical axis.
		VerticalAlignment Vertical = VerticalAlignment::Top;

		// What order the children are visited in.
		SortOrder Order = SortOrder::LayoutOrder;

		// How spare horizontal room is spent - growth or spacing.
		//
		// **Named by axis rather than by role, which is Roblox's shape.** In a
		// horizontal list this is the fill axis and `Fill` grows the children;
		// in a vertical list it is the cross axis and `Fill` stretches them.
		//
		// @since v0.17
		FlexAlignment HorizontalFlex = FlexAlignment::None;

		// How spare vertical room is spent. The other half of the pair above.
		//
		// @since v0.17
		FlexAlignment VerticalFlex = FlexAlignment::None;

		// Where each child sits across its own line.
		//
		// `Automatic` follows the cross-axis alignment - or stretches, when the
		// cross axis is flexed. A child's own `FlexItem` may override this.
		//
		// @since v0.17
		ItemLineAlignment ItemLine = ItemLineAlignment::Automatic;

		// Whether children start a new line instead of overflowing the fill
		// axis.
		//
		// @since v0.17
		bool Wraps = false;
	};

	// How one element trades size for spare room in a flexed list.
	//
	// A modifier on the *child* rather than on the layout, which is Roblox's
	// `UIFlexItem`: the list decides the default and this overrides it for one
	// element, which is what lets a toolbar hold one spring beside fixed
	// buttons.
	//
	// @since v0.17
	struct FlexItem {
		// How much of the spare room this element takes, against its
		// siblings' ratios. Read only when `Mode` is `Custom`.
		//
		// Zero, so a `Custom` item with untouched ratios flexes not at all -
		// the honest reading of ratios nobody set.
		float GrowRatio = 0.0f;

		// How much of an overflow this element absorbs. Read only when `Mode`
		// is `Custom`.
		float ShrinkRatio = 0.0f;

		// The fixed grow:shrink ratio, or `Custom` for the two fields above.
		FlexMode Mode = FlexMode::None;

		// Where this element sits across its line, overriding the layout's.
		ItemLineAlignment ItemLine = ItemLineAlignment::Automatic;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// Places the parent's children on a grid.
	//
	// @since v0.8
	struct GridLayout {
		// How big each cell is, against the parent's absolute size.
		core::UDim2 CellSize{0.0f, 100.0f, 0.0f, 100.0f};

		// Space between cells, on both axes.
		core::UDim2 CellPadding{0.0f, 5.0f, 0.0f, 5.0f};

		// How many cells before wrapping. Zero fits as many as the parent
		// holds, which is Roblox's meaning of its own default.
		int32_t MaxCells = 0;

		// Which way a row or column is filled.
		FillDirection Direction = FillDirection::Horizontal;

		// Which corner the fill starts from.
		StartCorner Corner = StartCorner::TopLeft;

		// Where the grid sits along the horizontal axis.
		HorizontalAlignment Horizontal = HorizontalAlignment::Left;

		// Where the grid sits along the vertical axis.
		VerticalAlignment Vertical = VerticalAlignment::Top;

		// What order the children are visited in.
		SortOrder Order = SortOrder::LayoutOrder;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// Makes the parent element draggable with a pointer.
	//
	// **A modifier on the element rather than a flag on it**, which is Roblox's
	// `UIDragDetector` and is the right shape here for the reason every other
	// modifier is one: almost nothing is draggable, and a field per drag setting
	// on `Element` would be that memory on every node in every interface.
	//
	// **The router writes `Element::Position`, and that is the whole feature.**
	// A drag is a gesture and a position is a property, so the only thing this
	// component decides is how one becomes the other - and what it produces is an
	// ordinary authored value a script reads back, a save file keeps and a
	// replica receives. Nothing here holds a "drag state" of its own;
	// `gui::Router` holds where a gesture is part-way through, exactly as it
	// holds the hover and the press.
	//
	// @since v0.18
	struct DragDetector {
		// An element the dragged one is kept inside, or null for no bound.
		ecs::Entity BoundingUI;

		// The line a `TranslateLine` drag runs along, in canvas pixels. Its
		// length does not matter; only its direction is read.
		core::Vector2 Axis{1.0f, 0.0f};

		// How far the element may be moved from where the drag started, in
		// pixels. **A translation and not a position**, so the same detector on
		// two elements bounds each of them relative to itself.
		//@{
		core::Vector2 MinTranslation{-1.0e6f, -1.0e6f};
		core::Vector2 MaxTranslation{1.0e6f, 1.0e6f};
		//@}

		// What the drag does. Roblox's default, and the one an author who added
		// a detector and set nothing else means: move it about freely.
		DragStyle Style = DragStyle::TranslatePlane;

		// Which half of the parent's `Position` it writes.
		DragResponse Response = DragResponse::Offset;

		// Whether the drag happens at all.
		bool Enabled = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		// `BoundingUI` aligns the whole component to eight.
		uint8_t Reserved[5] = {};
	};

	// Lays the parent's children out as rows, and *their* children as cells.
	//
	// **The only layout in this module that reaches two levels down**, and that
	// is what a table is: a row decides nothing about its own children's widths,
	// because a column has to be the same width in every row or the thing is not
	// a table. Roblox says the same in its own words - a `UITableLayout` "takes
	// control of sibling and cell elements' Size and Position".
	//
	// A cell's measured size is still what decides the column, so a
	// `UISizeConstraint` on a header cell sets that column's width for every row
	// under it. That is the idiom Roblox's own sample is built on.
	//
	// @since v0.18
	struct TableLayout {
		// Space between one row and the next, and one cell and the next.
		core::UDim2 Padding;

		// Whether the parent's children are rows stacked down, or columns
		// stacked across.
		FillDirection Direction = FillDirection::Vertical;

		// Where the whole table sits inside the parent.
		//@{
		HorizontalAlignment Horizontal = HorizontalAlignment::Left;
		VerticalAlignment Vertical = VerticalAlignment::Top;
		//@}

		// What order the rows and the cells are visited in.
		SortOrder Order = SortOrder::LayoutOrder;

		// Whether spare room is shared out among the columns and the rows.
		//
		// **Off by default, which is Roblox's and is the surprising half**: a
		// table whose cells do not fill the parent extends *past* it rather than
		// stretching to it, because a column's width is a fact about its content
		// until an author says otherwise.
		//@{
		bool FillEmptySpaceColumns = false;
		bool FillEmptySpaceRows = false;
		//@}

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// Shows one of the parent's children at a time and slides the rest aside.
	//
	// **The animation is here as of v0.17, and the clock is still not.** The
	// objection this entry carried was that a tween needs a frame time and `gui`
	// is L7 with none - which was the wrong shape of answer, because every other
	// module in this engine that needs a clock is *handed* one.
	// `render::FlipbookFrameAt` takes `seconds` and answers which frame is
	// showing; `assets::Grant::HasExpired` takes `nowSeconds`; `net` reads no
	// clock at all. So the page position is a **pure function of elapsed time**
	// rather than a state a tick advances, `CompileRequest::Seconds` is where
	// the time arrives, and this module still reads no clock.
	//
	// What that buys is the same thing it buys `Flipbook`: a carousel halfway
	// through a slide is a value a test states rather than one it waits for.
	//
	// **`PageMotion` is where the jump's start time lives**, and it is engine
	// state rather than a property - see that component.
	//
	// **`CurrentPage` is writable here and read-only in Roblox**, where it moves
	// through `JumpTo`, `Next` and `Previous`. A property a script can assign is
	// the same three methods without a method table, and it is what makes a page
	// a value a save file and a replica can carry.
	//
	// @since v0.18
	struct PageLayout {
		// The child being shown, or null for the first in order.
		ecs::Entity CurrentPage;

		// Space between one page and the next, in the fill direction.
		core::UDim Padding;

		// How long a slide takes, in seconds. Zero or less cuts.
		//
		// **Above the byte-sized members rather than among them**, which is the
		// alignment argument the `Reserved` field below carries in full.
		//
		// @since v0.17
		float TweenTime = 1.0f;

		// Which way the pages are stacked.
		FillDirection Direction = FillDirection::Horizontal;

		// What order the pages are in.
		SortOrder Order = SortOrder::LayoutOrder;

		// Whether the last page's neighbour is the first.
		//
		// **It changes where the pages *are*, not only what a script may jump
		// to.** With this set the strip wraps, so the page before the first is
		// drawn on the near side rather than off the far end - which is what
		// makes a carousel read as a loop while it is moving.
		bool Circular = false;

		// Whether changing `CurrentPage` slides or cuts.
		//
		// **Off cuts instantly and is not the same as a zero `TweenTime`**,
		// which also cuts: one says "this layout does not animate" and the other
		// says "animate over no time". They draw the same thing and read back
		// differently, which is Roblox's arrangement.
		//
		// @since v0.17
		bool Animated = true;

		// The curve the slide follows, and which end of it is eased.
		//
		// `core::EasingStyle` rather than a set of this module's own: the curve
		// is `core::TweenInfo::Ease` and a second enum meaning the same eleven
		// things is the debt this repository names most often.
		//
		// @since v0.17
		//@{
		core::EasingStyle Easing = core::EasingStyle::Back;
		core::EasingDirection EasingWay = core::EasingDirection::Out;
		//@}

		// Explicit padding, for the reason every other `Reserved` gives.
		// `CurrentPage` aligns the whole component to eight.
		//
		// **The six byte-sized members are grouped and `TweenTime` sits above
		// them**, which is what `ecs::AuditComponents` asks for and what it
		// caught twice here. `Entity` is a `uint64_t`, so this component aligns
		// to eight; a `float` left below the byte group put four bytes of tail
		// padding after it, and a component serialised as its object
		// representation carries those bytes into every save and every delta.
		// They differ between two runs of one scene, which is `just
		// determinism` failing for a reason no member explains.
		uint8_t Reserved[6] = {};
	};

	// Where a `UIPageLayout` is between two pages, and when it set off.
	//
	// **Engine state rather than a property, and local rather than replicated.**
	// It holds a moment on *this* process's monotonic clock, which means nothing
	// on another one - so it sits beside `ScrollState` on the list
	// `replication::DefaultReplicatedComponents` refuses, for the same reason:
	// what crosses is `CurrentPage`, and each end animates to it on its own
	// clock. A client that received a timestamp from a server would slide from
	// wherever that server's uptime happened to put it.
	//
	// Written by the layout when it notices `CurrentPage` has moved, which is
	// what makes assigning the property enough - there is no `JumpTo` to hook
	// and no setter this module owns.
	//
	// @since v0.17
	struct PageMotion {
		// The page the strip is sliding away from.
		ecs::Entity From;

		// The page it is sliding to, as of the last time the layout looked.
		// Not the same as `PageLayout::CurrentPage`: the difference between
		// them is how the layout knows a jump happened.
		ecs::Entity To;

		// `CompileRequest::Seconds` when the slide began. Negative means the
		// strip is at rest, which is what a layout that has never moved holds.
		double StartedAt = -1.0;

		// How far along the slide is, after the easing curve. Resolved once per
		// layout, before the walk, so the recursion that places the pages needs
		// no clock of its own.
		//
		// **Not clamped to one, deliberately.** `Back` and `Elastic` overshoot
		// by design and the overshoot is the whole visual point of them; a
		// clamp here would make the two curves that are worth choosing look
		// exactly like `Quad`.
		float Alpha = 1.0f;

		// Explicit padding, for the reason every other `Reserved` gives. The
		// two entities and the `double` align this to eight, so a lone `float`
		// leaves four bytes nothing writes.
		uint8_t Reserved[4] = {};
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

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// Clamps the parent's resolved size, in pixels.
	//
	// @since v0.8
	struct SizeLimits {
		// The smallest the parent may resolve to. Zero means unconstrained.
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
		// The range a scaled label may pick its size from.
		//
		// **One is the floor rather than zero**, because text at zero pixels is
		// an element that silently draws nothing rather than one that is
		// obviously too small.
		//@{
		int32_t Min = 1;
		int32_t Max = 100;
		//@}
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
		// What the outline looks like.
		//
		// **Its own transparency rather than the parent's**, so an outline can
		// stay solid on a fading element - which is what a focus ring wants and
		// what inheriting would make impossible.
		//@{
		core::Color3 Color{0.0f, 0.0f, 0.0f};
		float Thickness = 1.0f;
		float Transparency = 0.0f;
		//@}

		// Whether the outline is drawn at all.
		//
		// **A property rather than "set the transparency to one".** A game that
		// flashes a focus ring wants to put the colour and the thickness back
		// exactly as they were, and a script that had to remember the old
		// transparency to do it would be storing engine state in a table.
		//
		// @since v0.18
		bool Enabled = true;

		// What it outlines - the rectangle, or the content inside it.
		//
		// @since v0.18
		StrokeMode Apply = StrokeMode::Contextual;

		// How the corners are turned, and what `Thickness` is measured in.
		//
		// **These two took the padding rather than growing the row**, which is
		// the whole reason `Reserved` was there: a component in an archetype
		// store is a column, and two bytes that were already being paid for are
		// free where two more would cost a byte per element in every world that
		// has one.
		//
		// @since v0.17
		//@{
		LineJoin Join = LineJoin::Round;
		StrokeSizing Sizing = StrokeSizing::FixedSize;
		//@}
	};

	// Multiplies a ramp of colour and transparency into what the parent draws.
	//
	// **A modifier and not a fill, which is why it multiplies.** Roblox's
	// `UIGradient` tints whatever its parent already draws - a background, an
	// image and a text run alike - so a white gradient is invisible and a
	// gradient on a red panel ramps red. That is what lets one gradient sit under
	// an `ImageLabel` and shade the picture rather than covering it.
	//
	// **On a `UIStroke` it ramps that stroke instead.** The parent decides which,
	// and it is the same component either way, which is Roblox's arrangement -
	// an outline that fades along its length is a gradient parented one level
	// deeper rather than a second set of properties on the stroke.
	//
	// @since v0.18
	struct Gradient {
		// The colour ramp, sampled along the gradient's own axis.
		core::ColorSequence Color{core::Color3{1.0f, 1.0f, 1.0f}};

		// The transparency ramp, sampled along the same axis. **Added to what
		// the element already has rather than replacing it**, which is Roblox's
		// composition: an element at 0.5 under a gradient ramping 0 to 0.5 fades
		// from half to fully clear.
		core::NumberSequence Transparency{0.0f};

		// How far the ramp is slid along, in multiples of the parent's own size.
		//
		// A value of `(1, 0)` moves the whole ramp one element-width to the
		// right, which - depending on `Rotation` - can put all of it outside the
		// element and leave one flat colour.
		core::Vector2 Offset;

		// The ramp's clockwise rotation in degrees, starting left to right.
		//
		// **The ends still snap to the element's edges.** A rotated gradient
		// spans the element's extent *along the rotated axis*, so turning one by
		// ninety degrees ramps top to bottom over the full height rather than
		// over the width.
		float Rotation = 0.0f;

		// Whether the ramp is applied at all.
		bool Enabled = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[3] = {};
	};

	// Multiplies the parent's resolved size and text size.
	//
	// @since v0.8
	struct Scale {
		// What the parent's resolved size and text size are multiplied by.
		//
		// Applied after layout rather than during it, so scaling a container
		// does not re-flow what is inside it.
		float Factor = 1.0f;
	};

	// --- what the layout pass produces --------------------------------------

	// Where an element actually ended up.
	//
	// **Derived, and the only component here that is.** One pass writes it,
	// parent before child; the draw pass and the hit test read it with a query.
	// Nothing else in the engine may keep a second copy - `scene::Bounds` gives
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

		// How much room the string takes at `TextSize`, in canvas pixels.
		//
		// **The layout's estimate and not a backend's measurement**, which is
		// `Layout.hpp`'s standing rule: `AVERAGE_ADVANCE` is the one answer this
		// engine has, so `TextBounds` agrees with what `TextScaled` fitted and
		// with what a headless test asserts. A backend with real metrics would
		// disagree with both, which is why nothing asks one.
		//
		// @since v0.18
		core::Vector2 TextBounds;

		// How deep in the tree, counted from the collector. The tiebreak that
		// makes paint order total.
		int32_t Depth = 0;

		// The position in the compile's paint order.
		//
		// Assigned by `Compiled::Rebuild` and stored rather than left local so
		// that a panel or a test can ask why one element covered another.
		// `ElementsAt` is the reader that turned that from a convenience into a
		// contract: `PlayerGui:GetGuiObjectsAtPosition` answers front to back,
		// and this is how it does so without a second traversal deciding what is
		// on top.
		int32_t Order = 0;

		// Whether this element descends from an enabled collector and every
		// ancestor between was visible.
		//
		// **Derived by the pass and not maintained at the write**, exactly as
		// `scene::Visual`'s rendered flag is: ancestry is not local, so hooking
		// every parenting path is where one gets missed and an element draws
		// after being detached.
		bool Rendered = false;

		// Whether `TextBounds` fits inside `AbsoluteSize`.
		//
		// Roblox's `TextFits`, and it is stored rather than left for a caller to
		// derive so that the comparison happens once, in the pass that produced
		// both numbers.
		//
		// @since v0.18
		bool TextFits = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		uint8_t Reserved[2] = {};
	};

	// What every 3D adornment carries.
	//
	// **An adornment is a `GuiBase3d`, which is a description and not a
	// drawing.** It says what to outline, in what colour, and how solid - and
	// nothing here resolves that into geometry, because resolving it needs the
	// adornee's `CFrame` and stud extent and those are `scene::Transform` and
	// `scene::Bounds`. `gui` links neither and `gui/AGENTS.md` refuses the edge.
	//
	// That split is `D00022`'s, arrived at for a `SurfaceGui`'s canvas and the
	// same one word for word here: **whoever draws an adornment has both
	// operands, and this module has one.** What `gui` owns is the tree half -
	// which instance an adornment is about, and whether it is anywhere it may
	// be drawn from at all.
	//
	// @since v0.8
	struct Adornment {
		// What it is drawn around, or null to mean "my parent".
		//
		// **Null is a meaningful value rather than an unset one.** Roblox
		// resolves an unset `Adornee` to the adornment's parent, which is what
		// makes `SelectionBox` usable by parenting it to the thing it outlines
		// and setting nothing else. `AdorneeOf` is where that resolution
		// happens, once, rather than at each of the places that would otherwise
		// each have to remember it.
		ecs::Entity Adornee;

		// The outline colour.
		core::Color3 Color{0.0f, 0.65f, 1.0f};

		// 0 is opaque and 1 is invisible. Roblox's sense, kept for the reason
		// every other transparency here keeps it.
		float Transparency = 0.0f;

		// Draw order among adornments. Higher draws later, so on top.
		int32_t ZIndex = 0;

		// Whether it is drawn at all.
		bool Visible = true;

		// Whether it draws over the world rather than being occluded by it.
		//
		// **On by default, which is the opposite of a `Part`.** An adornment is
		// an editor affordance before it is a scene element: a selection box a
		// wall hides is a selection box that does not tell you what is
		// selected, which is the one thing it is for.
		bool AlwaysOnTop = true;

		// Explicit padding, for the reason every other `Reserved` gives.
		// `Adornee` aligns the whole component to eight.
		uint8_t Reserved[2] = {};
	};

	// The box a `SelectionBox` draws.
	//
	// Separate from `Adornment` because the handle adornments carry none of it:
	// a `SelectionBox` is an outline with an optional filled face, and a handle
	// is a grab target.
	//
	// @since v0.8
	struct SelectionOutline {
		// Width of the outline's world-space line segments, in studs.
		float LineThickness = 0.05f;

		// Colour of the optional filled faces.
		core::Color3 SurfaceColor{0.0f, 0.65f, 1.0f};

		// Zero is opaque and one omits the filled faces entirely.
		float SurfaceTransparency = 1.0f;
	};

	// Where a handle adornment sits relative to its adornee, and how big.
	//
	// @since v0.8
	struct HandleShape {
		// The offset from the adornee's own frame.
		core::CFrame Offset;

		// A positional offset scaled by the adornee's half extent, so one reaches
		// the corresponding surface.
		core::Vector3 SizeRelativeOffset;
	};

	// The extent of a box handle, in studs.
	//
	// @since v0.22
	struct BoxHandleShape {
		// Full width, height and depth.
		core::Vector3 Size{1.0f, 1.0f, 1.0f};
	};

	// The radius of a sphere handle, in studs.
	//
	// @since v0.22
	struct SphereHandleShape {
		// Distance from the centre to the wire surface.
		float Radius = 0.5f;
	};

	// The dimensions of a cylinder handle, in studs and degrees.
	//
	// @since v0.22
	struct CylinderHandleShape {
		// Outer radius.
		float Radius = 0.5f;

		// Inner radius. Zero makes a solid ring profile.
		float InnerRadius = 0.0f;

		// Length along the handle's local Z axis.
		float Height = 1.0f;

		// Portion of the circumference to draw, in degrees.
		float Angle = 360.0f;
	};

	// The dimensions of a line handle.
	//
	// @since v0.22
	struct LineHandleShape {
		// Length along the handle's local Z axis, in studs.
		float Length = 1.0f;

		// Width passed to the overlay drawer.
		float Thickness = 1.0f;
	};

	// The dimensions of a cone handle.
	//
	// @since v0.22
	struct ConeHandleShape {
		// Length from base to tip, in studs.
		float Height = 1.0f;

		// Radius of the circular base, in studs.
		float Radius = 0.5f;

		// Whether the base cap is omitted.
		bool Hollow = false;

		// Named padding for deterministic generated serialisation.
		uint8_t Reserved[3] = {};
	};

	// Which resize handles a `Handles` instance draws.
	//
	// Bits follow the six `NormalId` ordinals registered by this module.
	//
	// @since v0.22
	struct HandlesShape {
		// Enabled faces. The low six bits are used and default to all faces.
		uint32_t Faces = 0x3fu;
	};

	// Which rotation rings an `ArcHandles` instance draws.
	//
	// @since v0.22
	struct ArcHandlesShape {
		// Enabled axes: X, Y and Z in the low three bits.
		uint32_t Axes = 0x7u;
	};

	// What `GuiService` holds.
	//
	// **A component on the service instance rather than a resource**, which is
	// the opposite of what `ecs/AGENTS.md`'s one-of-a-kind rule usually asks
	// for - and the reason is that a service *is* an instance here. `scene`'s
	// services are rows in the tree that `GetService` finds by name, so their
	// state has to be reachable the same way a `Part`'s is: through a property
	// on the thing a script is holding. A resource would make
	// `GuiService.SelectedObject` a property with nothing behind it.
	//
	// @since v0.8
	struct GuiServiceState {
		// The element a gamepad or keyboard has selected, or null.
		//
		// **This is what makes `GuiObject::Selectable` mean something.** The
		// property has existed since the tree was registered and nothing read
		// it, which is the state the roadmap refuses to leave a property in -
		// `Select` and `SelectNext` are the readers.
		ecs::Entity SelectedObject;

		// The `TextBox` the keyboard is going to, or null.
		//
		// **The focus lives here rather than on `gui::Router`, because two
		// modules read it and only one decides it.** The router decides - a
		// press lands on a text box or somewhere else - and
		// `UserInputService:GetFocusedTextBox` at L9 reads, with no route to a
		// router at all. A copy held beside the decision would be rule 2's
		// second statement of one fact, and the two would part company the first
		// frame a box was destroyed.
		//
		// **A handle and never a pointer**, which is what makes a focused box
		// that has since been destroyed a question rather than a crash: the
		// generation in the id goes stale and `FocusedTextBox` answers null.
		//
		// @since v0.15
		ecs::Entity FocusedTextBox;

		// Whether a platform menu is covering the game.
		//
		// Set by a host, read by a script that wants to pause. Nothing in this
		// engine opens one yet; it is here because a script asking "is the menu
		// up" during a pause handler is the ordinary use and answering `false`
		// truthfully is better than not answering.
		bool MenuIsOpen = false;

		// Whether moving the selection is allowed to pick something on its own.
		//
		// Roblox's `AutoSelectGuiEnabled`. False means a game drives selection
		// itself and `SelectNext` refuses to seed one from nothing.
		bool AutoSelectGuiEnabled = true;

		// Explicit padding, for the reason every other `Reserved` gives. Both
		// handles align the whole component to eight.
		uint8_t Reserved[6] = {};
	};
}
