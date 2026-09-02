#include "Utf8.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/RichText.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace engine::gui {

	namespace {
		using core::Rect;
		using core::Vector2;
		using ecs::ClassId;
		using ecs::Entity;
		using ecs::Store;

		// How deep this file will descend before deciding it is looking at a
		// cycle rather than at a UI.
		//
		// `Store::SetParent` refuses to make a cycle, so nothing should ever
		// reach this. It is here for the reason `ecs/Instances.cpp` gives about
		// its own limit: a hang with a growing allocation behind it is the worst
		// way to find out.
		constexpr int MAXIMUM_DEPTH = 256;

		// How far past the fill axis a child may land before a wrapped list
		// breaks the line.
		//
		// Three children at a third of the parent each can sum a ULP over the
		// span, and wrapping the last one onto its own line for that is the
		// bug an author cannot see. A quarter pixel is invisible at any scale
		// a UI draws at.
		constexpr float WRAP_TOLERANCE = 0.25f;

		// The class ids this pass tests against, looked up once per process.
		//
		// **LayoutIds and not names.** `Store::IsA` is an ancestor scan over a handful
		// of integers; building a `core::Name` per node per frame would take the
		// process-wide registry's mutex once per element, which is the exact
		// cost `scene`'s `NormalIdEnum` comment measures.
		struct LayoutIds {
			ClassId Object;
			ClassId Collector;
			ClassId ScreenGui;
			ClassId SurfaceGui;
			ClassId BillboardGui;

			LayoutIds() {
				RegisterGuiClasses();
				Object = GuiClass("GuiObject");
				Collector = GuiClass("LayerCollector");
				ScreenGui = GuiClass("ScreenGui");
				SurfaceGui = GuiClass("SurfaceGui");
				BillboardGui = GuiClass("BillboardGui");
			}
		};

		const LayoutIds &LayoutClasses() {
			static const LayoutIds ids;
			return ids;
		}

		// The containers a collector may draw from, by name.
		//
		// **Names rather than class ids, and that is this module's existing
		// arrangement rather than a shortcut.** `Workspace`, `StarterGui` and a
		// player's `PlayerGui` are `scene`'s services, and `gui/AGENTS.md`
		// refuses an edge to `scene` - the same refusal that made
		// `SurfaceGui::Face` re-declare `NormalId`'s six members here, "pinned
		// by a test at each end, which is the arrangement `DefaultMaterial`'s
		// duplicated 'Plastic' already uses".
		//
		// So these three strings are duplicated from `scene/Services.cpp` and
		// `gui/tests/Layout.cpp` pins them against it. A rename on either side
		// fails a test rather than silently drawing nothing, which is the
		// failure this shape is chosen to make loud.
		struct LayoutContainers {
			core::Name Workspace{WORKSPACE};
			core::Name StarterGui{STARTER_GUI};
			core::Name PlayerGui{PLAYER_GUI};
		};

		const LayoutContainers &LayoutRoots() {
			static const LayoutContainers names;
			return names;
		}

		// Whether a collector sits somewhere it is allowed to draw from.
		//
		// **Roblox's rule, and it is a containment rule rather than a style
		// one.** A `ScreenGui` parented to a `Part` draws nothing - not because
		// it is invisible but because nothing is looking at that part of the
		// tree - and an engine that drew it anyway would let an author ship a
		// game whose interface appears in the studio and not in the client.
		//
		//   - a `ScreenGui` draws from **`StarterGui`** or from a player's
		//     **`PlayerGui`**. The studio shows the first; a client shows the
		//     second, which is the copy Roblox makes when a player spawns.
		//   - a `SurfaceGui` or a `BillboardGui` draws from those *and* from
		//     **`Workspace`**, because both are attached to something in the
		//     world and the world is where that something lives.
		//
		// Walked upward rather than tested against the immediate parent: a
		// `ScreenGui` inside a `Folder` inside `StarterGui` is contained, and
		// authors nest.
		//
		// @param collector The `LayerCollector` being considered.
		// @param spatial   Whether `Workspace` counts, which is true for the
		//        two collectors that hang off something in the world.
		bool Contained(const Store &store, Entity collector, bool spatial) {
			const LayoutContainers &roots = LayoutRoots();

			for (Entity above = store.ParentOf(collector); above != ecs::NULL_ENTITY;
				 above = store.ParentOf(above)) {
				const core::Name name = store.InstanceNameOf(above);

				if (name == roots.StarterGui || name == roots.PlayerGui) {
					return true;
				}
				if (spatial && name == roots.Workspace) {
					return true;
				}
			}

			return false;
		}

		Rect FromCorner(const Vector2 &topLeft, const Vector2 &size) {
			return Rect{topLeft, Vector2{topLeft.X + size.X, topLeft.Y + size.Y}};
		}

		// The parent extents a `UDim2` resolves against, per `SizeConstraint`.
		//
		// `RelativeXX` feeding the parent's *width* to both axes is what keeps
		// a square element square as its parent changes shape - which is the
		// whole reason the property exists, and is easy to write backwards.
		Vector2 Basis(SizeConstraint constraint, const Vector2 &parent) {
			switch (constraint) {
			case SizeConstraint::RelativeXY:
				return parent;
			case SizeConstraint::RelativeXX:
				return Vector2{parent.X, parent.X};
			case SizeConstraint::RelativeYY:
				return Vector2{parent.Y, parent.Y};
			}
			return parent;
		}

		// Everything the layout of a container needs from its modifiers.
		struct Modifiers {
			const Padding *Inset = nullptr;
			const ListLayout *List = nullptr;
			const GridLayout *Grid = nullptr;
			const TableLayout *Table = nullptr;
			const PageLayout *Page = nullptr;

			// **Which instance `Page` came off, because the motion lives
			// there.** Every other modifier is read and thrown away; this one
			// has state beside it - `PageMotion` - and `AdvancePages` writes
			// that onto the `UIPageLayout` rather than onto the container it
			// modifies. Carrying the handle is what stops `RunPages` looking
			// for it on the parent and quietly finding nothing, which is
			// exactly the shape the first draft of this had: an animation that
			// resolved correctly, was never read, and left every existing case
			// passing.
			ecs::Entity PageOn;
			const AspectRatio *Aspect = nullptr;
			const SizeLimits *Limits = nullptr;
			const TextSizeLimits *TextLimits = nullptr;
			const Scale *Factor = nullptr;

			// Read by the *parent's* list layout rather than by this node's
			// own placement - a `UIFlexItem` describes how its parent trades
			// size for the line's spare room.
			const FlexItem *Flex = nullptr;
		};

		// Everything one walk of a node's child list produces.
		//
		// **The walk is the expensive thing in this file, so there is exactly
		// one of them per element.** `Store::EachChild` follows an intrusive
		// `FirstChild`/`NextSibling` chain through a `std::function`, which is a
		// type-erased call and a pointer chase into another table per child -
		// and both halves of laying a node out want that list. Measuring it
		// needs its scale, aspect and limits; placing it needs its padding, its
		// layout, and the children themselves.
		//
		// Splitting those into two walks was the last structural cost in
		// `engine.gui.bench.interface`: every element in the tree was chasing
		// its child chain twice per frame, once from its parent's measure loop
		// and once from its own `Place`. Producing both answers at once is the
		// whole of this struct.
		struct Scan {
			// The modifier children, which are the ones with no `Element`.
			Modifiers Mods;

			// Where this node's `GuiObject` children sit in the arena, as an
			// index and a count rather than pointers or a span.
			//
			// **Indices, because the arena grows while this is held.** Measuring
			// a sibling appends that sibling's children, which may reallocate -
			// so anything holding a pointer into it would dangle halfway through
			// the measure loop, on trees large enough to force a reallocation
			// and not on the small ones a test builds.
			size_t ChildFirst = 0;
			uint32_t ChildCount = 0;
		};

		// One child of a container, with what the layout needs to place it.
		struct Item {
			Entity Node;
			Vector2 Size;
			int32_t Order = 0;

			// Only filled in when the container sorts by name.
			//
			// **`InstanceNameOf` reaches the process-wide name registry**, which
			// is a shared lock and a hash lookup - and `SortOrder::Name` is the
			// rarest of the three. Fetching it for every child of every
			// container on every frame paid that cost for a field two sort
			// orders never read.
			core::Name Label;

			// What measuring this child found, carried through to placing it.
			Scan Found;
		};

		// The `GuiObject` children of every node currently being laid out.
		//
		// **A stack, and the discipline is exact.** `Place` on a node marks the
		// arena, measures each of its children - which appends each child's own
		// children - places them in turn, and then releases back to the mark.
		// A child's run therefore outlives the whole measure loop, which is what
		// it has to do: the run is read when that child is *placed*, long after
		// its siblings were measured on top of it.
		//
		// `thread_local` for `ScratchAt`'s reason: a `Store` binds its owning
		// thread, so two worlds laying out at once are two threads.
		std::vector<Entity> &ChildArena() {
			static thread_local std::vector<Entity> arena;
			return arena;
		}

		// Returns the arena to where it was, however the scope exits.
		//
		// **RAII rather than a `resize` at the end, because the alternative
		// fails silently and unboundedly.** `Place` already has three early
		// returns and will grow more; one of them added below the mark and above
		// the release would leak a child run per element per frame, for the life
		// of the process. Nothing would crash, no test would fail, and the only
		// symptom would be a session that slowly ate memory - which is the worst
		// way to find out, and the same reasoning `MAXIMUM_DEPTH` gives a few
		// lines above about hangs.
		//
		// The destructor cannot be skipped, so the invariant holds by
		// construction rather than by every future edit remembering it.
		class ArenaScope {
		  public:
			explicit ArenaScope(std::vector<Entity> &arena) : Storage(arena), Mark(arena.size()) {}

			~ArenaScope() {
				Storage.resize(Mark);
			}

			ArenaScope(const ArenaScope &) = delete;
			ArenaScope &operator=(const ArenaScope &) = delete;
			ArenaScope(ArenaScope &&) = delete;
			ArenaScope &operator=(ArenaScope &&) = delete;

		  private:
			std::vector<Entity> &Storage;
			size_t Mark;
		};

		// Walks one node's children once, sorting them into the two things any
		// caller wants: the modifiers, and the `GuiObject`s a layout arranges.
		//
		// **The split is `Element`, and that is a rule rather than a shortcut.**
		// A `UIPadding` under a frame is a child in the tree, and a list layout
		// that stacked it would leave a blank row where the modifier was. "Does
		// it have an `Element`" is exactly what makes something a `GuiObject`,
		// so the same test that finds the children rules out all eight modifier
		// lookups for each of them - which matters because the common container
		// is a frame full of frames and eight misses against one hit is the
		// ratio in every real interface.
		Scan ScanChildren(const Store &store, Entity instance, std::vector<Entity> &arena) {
			Scan found;
			found.ChildFirst = arena.size();

			store.EachChild(instance, [&](Entity child) {
				if (store.Get<Element>(child) != nullptr) {
					arena.push_back(child);
					return;
				}

				if (found.Mods.Inset == nullptr) {
					found.Mods.Inset = store.Get<Padding>(child);
				}
				if (found.Mods.List == nullptr) {
					found.Mods.List = store.Get<ListLayout>(child);
				}
				if (found.Mods.Grid == nullptr) {
					found.Mods.Grid = store.Get<GridLayout>(child);
				}
				if (found.Mods.Table == nullptr) {
					found.Mods.Table = store.Get<TableLayout>(child);
				}
				if (found.Mods.Page == nullptr) {
					found.Mods.Page = store.Get<PageLayout>(child);
					if (found.Mods.Page != nullptr) {
						found.Mods.PageOn = child;
					}
				}
				if (found.Mods.Aspect == nullptr) {
					found.Mods.Aspect = store.Get<AspectRatio>(child);
				}
				if (found.Mods.Limits == nullptr) {
					found.Mods.Limits = store.Get<SizeLimits>(child);
				}
				if (found.Mods.TextLimits == nullptr) {
					found.Mods.TextLimits = store.Get<TextSizeLimits>(child);
				}
				if (found.Mods.Factor == nullptr) {
					found.Mods.Factor = store.Get<Scale>(child);
				}
				if (found.Mods.Flex == nullptr) {
					found.Mods.Flex = store.Get<FlexItem>(child);
				}
			});

			found.ChildCount = static_cast<uint32_t>(arena.size() - found.ChildFirst);
			return found;
		}

		// What a container's padding takes out of each edge.
		//
		// Split out of `ContentArea` because the measure phase needs the same
		// four numbers and has no `Rect` yet - an automatically sized container
		// is being asked how big to be, so its rectangle is the answer rather
		// than the input. One function, so the two phases cannot disagree about
		// what a scale padding resolves against.
		struct Insets {
			float Left = 0.0f;
			float Right = 0.0f;
			float Top = 0.0f;
			float Bottom = 0.0f;
		};

		Insets InsetsOf(const Padding *padding, const Vector2 &size) {
			Insets out;
			if (padding != nullptr) {
				out.Left = padding->Left.Resolve(size.X);
				out.Right = padding->Right.Resolve(size.X);
				out.Top = padding->Top.Resolve(size.Y);
				out.Bottom = padding->Bottom.Resolve(size.Y);
			}
			return out;
		}

		// Declared here and defined below, because a scrolling frame's automatic
		// canvas measures its children with the same function an automatically
		// sized element does - and that function needs the layout helpers this
		// one sits above. One declaration is cheaper than moving either.
		Vector2 ContentExtent(
			const Store &store, const Scan &scan, const Vector2 &basis, int depth, std::vector<Entity> &arena
		);

		// Whether a bar's strip is taken out of the window.
		//
		// Three answers rather than two, and the third is the one worth having:
		// `Always` reserves the strip whether or not a bar is showing, which is
		// what stops a list jumping sideways the moment it grows past the frame.
		float BarInset(ScrollBarInset mode, bool showing, float thickness) {
			switch (mode) {
			case ScrollBarInset::None:
				return 0.0f;
			case ScrollBarInset::ScrollBar:
				return showing ? thickness : 0.0f;
			case ScrollBarInset::Always:
				return thickness;
			}
			return 0.0f;
		}

		// One thumb's rectangle along a track.
		//
		// **The length is the window's share of the canvas and the position is
		// the scroll's share of the travel**, which is the only pair of rules
		// under which a thumb at the bottom of its track means the canvas is at
		// the end of its range. Floored at the bar's own thickness so a very long
		// canvas still leaves something grabbable.
		//
		// @return The offset along the track and the thumb's length.
		std::pair<float, float>
		ThumbAlong(float track, float window, float canvas, float scrolled, float thickness) {
			const float length =
				canvas > 0.0f ? std::clamp(track * window / canvas, thickness, track) : track;
			const float travel = std::max(track - length, 0.0f);
			const float range = std::max(canvas - window, 0.0f);
			const float offset = range > 0.0f ? travel * std::clamp(scrolled / range, 0.0f, 1.0f) : 0.0f;
			return {offset, length};
		}

		// The rectangle a container's children are placed inside.
		//
		// The container's own rectangle, less its padding, less any scroll
		// offset. A scrolling frame's canvas is larger than the frame and moves
		// under it, which is the whole of what scrolling is here.
		//
		// **It also produces the frame's `ScrollState`, and that is deliberate
		// rather than convenient.** The canvas extent, the window after the bars
		// are inset, and the two thumb rectangles are all the same arithmetic
		// this function already does to place the children - and three consumers
		// need them: a script, the compile's bars and the router's drag. Working
		// them out a second time somewhere else is the failure `Resolved` exists
		// to refuse, and here it shows up as a thumb you can see and cannot grab.
		//
		// @param state Filled when `instance` scrolls, untouched otherwise.
		// @return Whether `state` was written.
		bool ContentArea(
			const Store &store,
			Entity instance,
			const Rect &own,
			const Modifiers &modifiers,
			const Scan &scan,
			int depth,
			std::vector<Entity> &arena,
			Rect &out,
			ScrollState &state
		) {
			Vector2 size = own.Size();
			Vector2 origin = own.Min;

			{
				const Insets insets = InsetsOf(modifiers.Inset, size);
				origin = Vector2{origin.X + insets.Left, origin.Y + insets.Top};
				size = Vector2{
					size.X - insets.Left - insets.Right,
					size.Y - insets.Top - insets.Bottom,
				};
			}

			const Scrolling *scrolling = store.Get<Scrolling>(instance);
			if (scrolling == nullptr) {
				out = FromCorner(origin, size);
				return false;
			}

			const Vector2 padded = size;
			const auto axes = static_cast<uint8_t>(scrolling->Direction);
			const bool scrollsX = (axes & static_cast<uint8_t>(ScrollingDirection::X)) != 0;
			const bool scrollsY = (axes & static_cast<uint8_t>(ScrollingDirection::Y)) != 0;
			const float thickness = static_cast<float>(std::max(scrolling->BarThickness, 0));

			// **The content is measured once, against the un-inset window.** It
			// cannot be measured against the final one, because the final one
			// depends on which bars show and that depends on the content - so
			// something has to be measured first and this is the choice that
			// keeps the pass single. A list whose rows are fixed-size, which is
			// every list this property is for, measures the same either way.
			Vector2 automatic;
			const bool growsX = scrolling->AutomaticCanvas == AutomaticSize::X ||
								scrolling->AutomaticCanvas == AutomaticSize::XY;
			const bool growsY = scrolling->AutomaticCanvas == AutomaticSize::Y ||
								scrolling->AutomaticCanvas == AutomaticSize::XY;
			if ((growsX || growsY) && depth < MAXIMUM_DEPTH) {
				automatic = ContentExtent(store, scan, padded, depth + 1, arena);
			}

			// Twice: once to find out which bars show, once with their strips
			// taken out. A third pass would change nothing - taking a strip out
			// can only make a bar *more* necessary, never less, so the second
			// answer is stable.
			Vector2 window = padded;
			Vector2 canvas;
			for (int pass = 0; pass < 2; pass++) {
				canvas = scrolling->CanvasSize.Resolve(window);
				if (growsX) {
					canvas.X = automatic.X;
				}
				if (growsY) {
					canvas.Y = automatic.Y;
				}

				const bool showsY = scrollsY && canvas.Y > window.Y;
				const bool showsX = scrollsX && canvas.X > window.X;
				window = Vector2{
					padded.X - BarInset(scrolling->VerticalInset, showsY, thickness),
					padded.Y - BarInset(scrolling->HorizontalInset, showsX, thickness),
				};
			}

			window = Vector2{std::max(window.X, 0.0f), std::max(window.Y, 0.0f)};

			// **Clamped here and not written back.** Roblox clamps
			// `CanvasPosition` on assignment; this module has no setter to hook -
			// the class table writes the field - so the clamp lives at the one
			// reader that offsets by it, and `Router::Update` clamps what *it*
			// writes. A script that assigns past the end therefore reads back
			// what it wrote and still sees the canvas stop where it should.
			//
			// **The rubber band is added after the clamp and not folded into
			// it**, which is the whole point of it: the clamp is where the
			// canvas is *allowed* to stop and the overshoot is how far past
			// that a hand has dragged it. Adding it here rather than widening
			// the clamp means a script still reads `CanvasPosition` back inside
			// the range while the frame is visibly past its end - which is
			// Roblox's behaviour and the only one that keeps a script's idea of
			// the canvas from lurching every time somebody flicks it.
			Vector2 pull;
			if (const ScrollMotion *motion = store.Get<ScrollMotion>(instance)) {
				pull = motion->Overshoot;
			}

			const Vector2 scrolled{
				std::clamp(scrolling->CanvasPosition.X, 0.0f, std::max(canvas.X - window.X, 0.0f)) + pull.X,
				std::clamp(scrolling->CanvasPosition.Y, 0.0f, std::max(canvas.Y - window.Y, 0.0f)) + pull.Y,
			};

			state = ScrollState{};
			state.CanvasSize = canvas;
			state.WindowSize = window;

			const bool showsY = scrollsY && canvas.Y > window.Y && thickness > 0.0f;
			const bool showsX = scrollsX && canvas.X > window.X && thickness > 0.0f;
			const Vector2 corner{origin.X, origin.Y};

			if (showsY) {
				const auto [offset, length] =
					ThumbAlong(std::max(window.Y, 0.0f), window.Y, canvas.Y, scrolled.Y, thickness);
				const float left =
					scrolling->VerticalBar == BarPosition::Left ? corner.X : corner.X + padded.X - thickness;
				state.VerticalThumb = Rect{
					Vector2{left, corner.Y + offset},
					Vector2{left + thickness, corner.Y + offset + length},
				};
			}

			if (showsX) {
				const auto [offset, length] =
					ThumbAlong(std::max(window.X, 0.0f), window.X, canvas.X, scrolled.X, thickness);
				const float shift = scrolling->VerticalBar == BarPosition::Left && showsY ? thickness : 0.0f;
				state.HorizontalThumb = Rect{
					Vector2{corner.X + shift + offset, corner.Y + padded.Y - thickness},
					Vector2{corner.X + shift + offset + length, corner.Y + padded.Y},
				};
			}

			// The canvas is at least the window, so a frame whose content does
			// not fill it still places children against the whole rectangle.
			out = FromCorner(
				Vector2{origin.X - scrolled.X, origin.Y - scrolled.Y},
				Vector2{std::max(canvas.X, window.X), std::max(canvas.Y, window.Y)}
			);
			return true;
		}

		// Applies the constraints that reshape a resolved size.
		//
		// Order is load-bearing: the scale multiplies what the `UDim2` said,
		// the aspect ratio reshapes that, and the size limits clamp the result.
		// Clamping before the aspect ratio would produce a rectangle that obeys
		// neither, which is the arrangement that reads as "the constraint does
		// not work".
		Vector2 Constrain(Vector2 size, const Modifiers &modifiers, const Vector2 &parent) {
			if (modifiers.Factor != nullptr) {
				size = size * modifiers.Factor->Factor;
			}

			if (const AspectRatio *aspect = modifiers.Aspect; aspect != nullptr && aspect->Ratio > 0.0f) {
				if (aspect->Type == AspectType::ScaleWithParentSize) {
					// The parent's extent along the dominant axis decides both,
					// so the element keeps its shape as the parent resizes
					// rather than as its own `UDim2` resolves.
					const float span = aspect->Dominant == DominantAxis::Width ? parent.X : parent.Y;
					size = aspect->Dominant == DominantAxis::Width ? Vector2{span, span / aspect->Ratio}
																   : Vector2{span * aspect->Ratio, span};
				} else if (aspect->Dominant == DominantAxis::Width) {
					size.Y = size.X / aspect->Ratio;
				} else {
					size.X = size.Y * aspect->Ratio;
				}
			}

			if (const SizeLimits *limits = modifiers.Limits) {
				size.X = std::clamp(size.X, limits->Min.X, limits->Max.X);
				size.Y = std::clamp(size.Y, limits->Min.Y, limits->Max.Y);
			}

			return size;
		}

		// The em size a label is actually drawn at.
		//
		// See `Layout.hpp`: the advance is an estimate and this is the one
		// answer everything downstream uses. Returning the authored size
		// unchanged when `Scaled` is off is not a shortcut - a scaled size on an
		// unscaled label would silently override what the author typed.
		// How many characters a label actually draws.
		//
		// **After the markup is stripped and after the visible limit**, because
		// both change what a reader sees and `TextScaled` fits what a reader
		// sees. A label whose text is mostly tags would otherwise shrink to fit
		// a string nobody is shown.
		//
		// **Characters and not bytes**, which is the same crossing
		// `Entry::CursorPosition` makes: `AVERAGE_ADVANCE` is a fraction of an em
		// per *glyph*, so counting the three bytes of an accented letter three
		// times measures a word half again too wide.
		size_t DrawnCharacters(const Label &label) {
			std::string plain;
			std::vector<DrawSpan> spans;
			std::string_view text = label.Text;
			if (label.Rich) {
				ParseRichText(label.Text, label, plain, spans);
				text = plain;
			}
			return Characters(FirstCharacters(text, label.MaxVisible));
		}

		// How many lines a label occupies, counting only the breaks it carries.
		//
		// **Wrapping is deliberately not modelled here.** Where a line breaks
		// depends on the width the element ends up with, which is what this is
		// helping to decide; `Layout.hpp`'s note on the single pass is the same
		// argument one property along. A wrapped label therefore reports the
		// bounds of its unwrapped text, which is the honest answer to "how much
		// room does this string want".
		size_t TextLines(std::string_view text) {
			return static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
		}

		// @param factor `UIScale`'s multiplier, or one where there is none.
		int32_t
		FittedTextSize(const Label &label, const Vector2 &box, const TextSizeLimits *limits, float factor) {
			// **`UIScale` multiplies the glyphs as well as the box**, which is
			// what `gui::Scale` says it does. It was applied in `Constrain` to
			// the rectangle and nowhere to the text, so a scale of two gave a
			// label twice the size with its writing at the authored size - and
			// only when `TextScaled` was off, because a scaled fit re-derives
			// from the enlarged box and happens to come out right. That is why
			// this hid: it was correct in the case people test with.
			float size = static_cast<float>(label.Size) * (factor > 0.0f ? factor : 1.0f);

			if (label.Scaled) {
				const size_t characters = std::max<size_t>(DrawnCharacters(label), 1);
				const float byWidth = box.X / (static_cast<float>(characters) * AVERAGE_ADVANCE);
				const float byHeight = box.Y / LINE_SPACING;

				size = std::min({byWidth, byHeight, size});
			}

			// Floored rather than rounded. A size that rounds up is a size that
			// does not fit, which is the one outcome this is for.
			int32_t fitted = static_cast<int32_t>(std::floor(size));

			// **The limits are last and are not scaled**, because they are
			// authored in the pixels the text ends up at: a maximum of 24 means
			// "never larger than 24 on screen", and scaling it with the element
			// would make it a limit on the authored size instead.
			if (limits != nullptr) {
				fitted = std::clamp(fitted, limits->Min, limits->Max);
			}

			// Never zero: a label drawn at nothing is indistinguishable from one
			// that failed to lay out.
			return std::max(fitted, 1);
		}

		size_t Place(
			Store &store,
			Entity instance,
			const Rect &area,
			const Rect &clip,
			int depth,
			float rotation,
			const Scan &scan
		);

		Vector2 Measure(
			const Store &store,
			Entity instance,
			const Vector2 &parent,
			int depth,
			Scan &scan,
			std::vector<Entity> &arena
		);

		// How many cells a grid puts on one line.
		//
		// Shared by the placement and by the measure, because a grid that
		// wrapped at one count while being sized and another while being placed
		// would produce a container the wrong number of rows tall - which reads
		// as the last row falling off the bottom rather than as an arithmetic
		// disagreement between two functions.
		int32_t CellsPerLine(const GridLayout &layout, float track, float cell, float gap) {
			if (layout.MaxCells > 0) {
				return layout.MaxCells;
			}

			// As many as fit, at least one. Zero would divide by nothing below
			// and is the value Roblox's own default carries.
			const float step = cell + gap;
			const int32_t fits = step > 0.0f ? static_cast<int32_t>((track + gap) / step) : 1;
			return std::max(fits, 1);
		}

		// How much room what is inside a container actually takes.
		//
		// **The second phase `AutomaticSize` needs, and the only one in this
		// file.** Everything else here is a single top-down sweep; a container
		// that grows to its content cannot be, because its size is a function of
		// its children's sizes and theirs are a function of its.
		//
		// Accumulated rather than collected: the extent of a stack is a sum and a
		// maximum, and of a grid a count - all of them single-pass. So this
		// measures each child, folds the answer in, and keeps no list, which is
		// what stops an automatically sized container allocating per frame.
		//
		// @param basis  What the children resolve their `UDim2` against - the
		//        container's own size with the automatic axes already zeroed and
		//        the padding already taken out.
		// @return The extent inside the padding. The caller adds it back.
		Vector2 ContentExtent(
			const Store &store, const Scan &scan, const Vector2 &basis, int depth, std::vector<Entity> &arena
		) {
			// **Marked here, after this node's own child run was appended.**
			// Measuring a child appends that child's children, and none of those
			// runs outlive this function - the placement pass measures again with
			// the final rectangle and produces its own. Without the release the
			// arena would carry a whole discarded generation per automatic
			// container per frame.
			const ArenaScope scope(arena);

			const Modifiers &modifiers = scan.Mods;

			if (modifiers.Grid != nullptr) {
				// **The cell decides both axes, so no child is measured at all**
				// - the same reason `RunGrid` does not consult `Measure`. What is
				// needed is the count, and how the count wraps.
				const Vector2 cell = modifiers.Grid->CellSize.Resolve(basis);
				const Vector2 gap = modifiers.Grid->CellPadding.Resolve(basis);
				const bool horizontal = modifiers.Grid->Direction == FillDirection::Horizontal;

				int32_t counted = 0;
				for (uint32_t index = 0; index < scan.ChildCount; index++) {
					const Element *child = store.Get<Element>(arena[scan.ChildFirst + index]);
					if (child != nullptr && child->Visible) {
						counted++;
					}
				}

				if (counted == 0) {
					return Vector2::Zero;
				}

				const int32_t perLine = CellsPerLine(
					*modifiers.Grid,
					horizontal ? basis.X : basis.Y,
					horizontal ? cell.X : cell.Y,
					horizontal ? gap.X : gap.Y
				);
				const int32_t onLine = std::min(counted, perLine);
				const int32_t lines = (counted + perLine - 1) / perLine;

				const float along = static_cast<float>(onLine) *
										((horizontal ? cell.X : cell.Y) + (horizontal ? gap.X : gap.Y)) -
									(horizontal ? gap.X : gap.Y);
				const float across = static_cast<float>(lines) *
										 ((horizontal ? cell.Y : cell.X) + (horizontal ? gap.Y : gap.X)) -
									 (horizontal ? gap.Y : gap.X);

				return horizontal ? Vector2{along, across} : Vector2{across, along};
			}

			const bool stacked = modifiers.List != nullptr;
			const bool horizontal = stacked && modifiers.List->Direction == FillDirection::Horizontal;

			// A wrapped list only wraps against a real span - a container
			// growing along its own fill axis has no edge to wrap at, so its
			// children stay one line, which is also what resolves the
			// circularity of "wrap against the width the wrapping decides".
			const float gap =
				stacked ? modifiers.List->Padding.Resolve(horizontal ? basis.X : basis.Y) : 0.0f;
			const float lineGap =
				stacked ? modifiers.List->Padding.Resolve(horizontal ? basis.Y : basis.X) : 0.0f;
			const float wrapSpan = horizontal ? basis.X : basis.Y;
			const bool wraps = stacked && modifiers.List->Wraps && wrapSpan > 0.0f;

			// The line being filled, and the lines already closed. For an
			// unwrapped list everything lands on the first line and the totals
			// reduce to the plain sum-and-maximum this always was.
			float lineAlong = 0.0f;
			float lineAcross = 0.0f;
			uint32_t lineCounted = 0;
			float widestLine = 0.0f;
			float closedAcross = 0.0f;
			uint32_t lines = 0;

			float along = 0.0f;
			float across = 0.0f;

			// Free positioning accumulates a bounding box instead, which is what
			// `along` and `across` hold when `stacked` is false.
			for (uint32_t index = 0; index < scan.ChildCount; index++) {
				const Entity node = arena[scan.ChildFirst + index];

				const Element *child = store.Get<Element>(node);
				if (child == nullptr || !child->Visible) {
					continue;
				}

				Scan found;
				const Vector2 size = Measure(store, node, basis, depth, found, arena);

				if (stacked) {
					const float mainSize = horizontal ? size.X : size.Y;
					const float extended = lineAlong + (lineCounted > 0 ? gap : 0.0f) + mainSize;

					// The same break `ScanLine` makes, against the same span,
					// or the measured height and the placed height disagree by
					// one line - the wrap arithmetic lives in two passes and
					// this is the half that sizes.
					if (wraps && lineCounted > 0 && extended > wrapSpan + WRAP_TOLERANCE) {
						widestLine = std::max(widestLine, lineAlong);
						closedAcross += lineAcross;
						lines++;
						lineAlong = mainSize;
						lineAcross = horizontal ? size.Y : size.X;
						lineCounted = 1;
						continue;
					}

					lineAlong = extended;
					lineAcross = std::max(lineAcross, horizontal ? size.Y : size.X);
					lineCounted++;
					continue;
				}

				// **The far edge, not the size**, because a child placed at an
				// offset needs room for the offset as well - and the near edge is
				// deliberately ignored: a child at a negative position hangs out
				// of its parent rather than pushing the parent's origin, which is
				// what keeps this a growth rule and not a reflow.
				const Vector2 anchored = child->Position.Resolve(basis);
				along = std::max(along, anchored.X + (1.0f - child->AnchorPoint.X) * size.X);
				across = std::max(across, anchored.Y + (1.0f - child->AnchorPoint.Y) * size.Y);
			}

			if (!stacked) {
				return Vector2{std::max(along, 0.0f), std::max(across, 0.0f)};
			}

			if (lineCounted > 0) {
				widestLine = std::max(widestLine, lineAlong);
				closedAcross += lineAcross;
				lines++;
			}

			along = widestLine;
			across = closedAcross;
			if (lines > 1) {
				across += lineGap * static_cast<float>(lines - 1);
			}

			return horizontal ? Vector2{along, across} : Vector2{across, along};
		}

		bool Grows(AutomaticSize automatic, bool horizontal) {
			if (automatic == AutomaticSize::XY) {
				return true;
			}
			return automatic == (horizontal ? AutomaticSize::X : AutomaticSize::Y);
		}

		// Resolves one node's size against the rectangle it sits in.
		//
		// Split from `Place` because a container running a list or a grid layout
		// has to know how big its children are *before* it can decide where they
		// go - which is the one thing that cannot be done in a single top-down
		// sweep and is why this function exists at all.
		//
		// @param scan Filled in with the whole of what the walk found - the
		//        modifiers this uses, and the child run `Place` will - so that
		//        placing the node afterwards costs no second walk.
		Vector2 Measure(
			const Store &store,
			Entity instance,
			const Vector2 &parent,
			int depth,
			Scan &scan,
			std::vector<Entity> &arena
		) {
			const Element *element = store.Get<Element>(instance);
			if (element == nullptr) {
				return Vector2::Zero;
			}

			scan = ScanChildren(store, instance, arena);
			Vector2 size = element->Size.Resolve(Basis(element->Constraint, parent));

			// **What a labelled element grows to is its string, and it is
			// measured with the same estimate that decides whether the string
			// fits.** That is the whole soundness argument and it is worth
			// stating, because this used to be a refusal on the grounds that
			// "growing a box to an estimate produces a box the text does not
			// fit".
			//
			// It does not, and the reason is the invariant `Layout.hpp` already
			// states: the backend draws at `Resolved::TextSize` and does not
			// second-guess it. Nothing downstream re-measures with real metrics,
			// so `AVERAGE_ADVANCE` is not an approximation of the truth - within
			// this engine it *is* the truth, the one answer a hit test, a
			// headless assertion and a renderer all agree on. A box grown to it
			// fits by the same definition of fitting the module uses everywhere
			// else.
			//
			// **`TextScaled` stays a no-op on a grown axis, by construction
			// rather than by a special case.** `FittedTextSize` divides the box
			// by exactly the product this multiplies, so it recovers the size it
			// started from: an author who sets both gets the size they asked for
			// in a box that holds it, which is the only reading under which the
			// pair means anything.
			//
			// The residual risk is that the estimate is wrong about real glyphs.
			// That risk is not introduced here - it is the same risk `TextScaled`
			// has carried since v0.8, and closing it means metrics shared below
			// L7 rather than a second opinion in this branch.
			const Label *label = store.Get<Label>(instance);

			if (element->Automatic != AutomaticSize::None && label != nullptr) {
				const bool growX = Grows(element->Automatic, true);
				const bool growY = Grows(element->Automatic, false);

				const Insets insets = InsetsOf(scan.Mods.Inset, size);

				// One line, because this module wraps nothing: `Label` carries a
				// string and a size and no wrap mode, so the height of a label
				// is one line's height whatever the width is. The day wrapping
				// arrives this is where the line count comes from, and it will
				// need the width *before* the height - which is why the two are
				// computed apart rather than as one extent.
				const auto characters = static_cast<float>(label->Text.size());
				const float advance = characters * AVERAGE_ADVANCE * static_cast<float>(label->Size);
				const float lineHeight = LINE_SPACING * static_cast<float>(label->Size);

				if (growX) {
					size.X = std::max(advance + insets.Left + insets.Right, 0.0f);
				}
				if (growY) {
					size.Y = std::max(lineHeight + insets.Top + insets.Bottom, 0.0f);
				}
			} else if (element->Automatic != AutomaticSize::None && depth < MAXIMUM_DEPTH) {
				const bool growX = Grows(element->Automatic, true);
				const bool growY = Grows(element->Automatic, false);

				// **The growing axes are zero for everything inside**, which is
				// the circularity this feature cannot avoid and Roblox's answer
				// to it: a child sized `{1, 0}` inside a parent sized by its
				// content is asking to be as wide as the thing it is deciding the
				// width of. Resolving that scale against zero makes the child
				// contribute nothing rather than diverging - so a scale-sized
				// child in an automatic parent measures as empty here, and then
				// fills the grown parent when it is placed.
				const Vector2 own{growX ? 0.0f : size.X, growY ? 0.0f : size.Y};

				const Insets insets = InsetsOf(scan.Mods.Inset, own);
				const Vector2 inner{
					own.X - insets.Left - insets.Right,
					own.Y - insets.Top - insets.Bottom,
				};

				// **`Scrolling` is not consulted, on purpose.** A scrolling
				// frame's canvas is larger than the frame by definition, and a
				// frame that grew to its own canvas would have nothing left to
				// scroll. Roblox splits the two properties for the same reason.
				const Vector2 content = ContentExtent(store, scan, inner, depth + 1, arena);

				if (growX) {
					size.X = std::max(content.X + insets.Left + insets.Right, 0.0f);
				}
				if (growY) {
					size.Y = std::max(content.Y + insets.Top + insets.Bottom, 0.0f);
				}
			}

			// After the growth rather than before it, so a `UISizeConstraint` on
			// an automatic container is a bound on what it grows to - which is
			// the only reading under which putting both on one element means
			// anything.
			return Constrain(size, scan.Mods, parent);
		}

		// The item list one level of the walk uses, reused across frames.
		//
		// **The recursion is depth first, so exactly one list is live at each
		// depth at any moment**: `Place` at depth *n* holds its children while
		// it recurses into one of them at depth *n+1*, and that child's own list
		// is a different level. One vector per level therefore serves the whole
		// walk, and because they keep their capacity between frames, a tree
		// whose shape has settled stops allocating entirely. Before this, every
		// container heap-allocated an item list on every frame.
		//
		// Sized to `MAXIMUM_DEPTH` once rather than grown on demand, because
		// growing it would reallocate the outer vector and dangle the reference
		// an outer level of the recursion is still holding - a bug that would
		// appear only on trees deeper than whatever the pool happened to have
		// reached.
		//
		// `thread_local` because a `Store` binds its owning thread, so two
		// worlds laying out at once are two threads and must not share this.
		std::vector<Item> &ScratchAt(int depth) {
			static thread_local std::vector<std::vector<Item>> pool(static_cast<size_t>(MAXIMUM_DEPTH) + 2);
			return pool[static_cast<size_t>(std::clamp(depth, 0, MAXIMUM_DEPTH + 1))];
		}

		// Measures the children this node's own scan already found.
		//
		// **No walk here, which is the point.** The child run was collected when
		// this node was measured; all that is left is to read it and measure
		// each entry, which is what appends the *grandchildren* for the level
		// below.
		//
		// @param scan  What scanning this node produced. Its child run is read
		//        by index rather than held as a span, because measuring each
		//        child appends to the same arena and may reallocate it.
		// @param named Whether the container's sort order reads `Item::Label`.
		//        Only `SortOrder::Name` does, and the lookup is a trip through
		//        the process-wide name registry - so it is asked for rather than
		//        taken.
		// @param items Cleared and filled. Owned by the caller so that the
		//        per-level scratch above can be handed in.
		void ChildItems(
			const Store &store,
			const Scan &scan,
			const Vector2 &area,
			int depth,
			bool named,
			std::vector<Item> &items,
			std::vector<Entity> &arena
		) {
			items.clear();
			items.reserve(scan.ChildCount);

			for (uint32_t index = 0; index < scan.ChildCount; index++) {
				const Entity child = arena[scan.ChildFirst + index];

				const Element *element = store.Get<Element>(child);
				if (element == nullptr || !element->Visible) {
					continue;
				}

				Item item;
				item.Node = child;
				item.Order = element->LayoutOrder;
				if (named) {
					item.Label = store.InstanceNameOf(child);
				}
				item.Size = Measure(store, child, area, depth, item.Found, arena);
				items.push_back(item);
			}
		}

		// Whether a container's sort order needs each child's name.
		bool SortsByName(const Modifiers &modifiers) {
			if (modifiers.List != nullptr) {
				return modifiers.List->Order == SortOrder::Name;
			}
			if (modifiers.Grid != nullptr) {
				return modifiers.Grid->Order == SortOrder::Name;
			}
			if (modifiers.Table != nullptr) {
				return modifiers.Table->Order == SortOrder::Name;
			}
			if (modifiers.Page != nullptr) {
				return modifiers.Page->Order == SortOrder::Name;
			}
			// A container with neither runs no sort at all - its children are
			// placed by their own `UDim2` - so no name is read.
			return false;
		}

		void Sort(std::vector<Item> &items, SortOrder order) {
			// `std::stable_sort`, so children that compare equal keep their
			// insertion order - which is what `Custom` means and is also the
			// only sensible tiebreak for the other two.
			switch (order) {
			case SortOrder::Name:
				std::stable_sort(items.begin(), items.end(), [](const Item &left, const Item &right) {
					return left.Label.Text() < right.Label.Text();
				});
				break;
			case SortOrder::LayoutOrder:
				std::stable_sort(items.begin(), items.end(), [](const Item &left, const Item &right) {
					return left.Order < right.Order;
				});
				break;
			case SortOrder::Custom:
				break;
			}
		}

		float AlignedStart(float span, float content, int alignment) {
			switch (alignment) {
			case 1:
				return (span - content) * 0.5f;
			case 2:
				return span - content;
			default:
				return 0.0f;
			}
		}

		// The grow and shrink weights one child brings to its line.
		struct FlexFactors {
			float Grow = 0.0f;
			float Shrink = 0.0f;
		};

		// What a child flexes as, from its own `UIFlexItem` else the layout.
		//
		// **A `FlexItem` overrides the layout even at `None`** - Roblox reads
		// that member as "unaffected", which is what lets one fixed button sit
		// in a `Fill` row. Without one, `Fill` on the fill axis means every
		// child grows and shrinks alike; the spacing members size nothing.
		FlexFactors FactorsFor(const FlexItem *flex, FlexAlignment mainFlex) {
			if (flex == nullptr) {
				return mainFlex == FlexAlignment::Fill ? FlexFactors{1.0f, 1.0f} : FlexFactors{};
			}

			switch (flex->Mode) {
			case FlexMode::None:
				return {};
			case FlexMode::Grow:
				return {1.0f, 0.0f};
			case FlexMode::Shrink:
				return {0.0f, 1.0f};
			case FlexMode::Fill:
				return {1.0f, 1.0f};
			case FlexMode::Custom:
				// Clamped because a negative ratio would grow on shrink and
				// shrink on grow, which no author means and CSS also refuses.
				return {std::max(flex->GrowRatio, 0.0f), std::max(flex->ShrinkRatio, 0.0f)};
			}
			return {};
		}

		// Where a child sits across its line, after every override has spoken:
		// its own `UIFlexItem` first, the layout's `ItemLineAlignment` second,
		// and `Automatic` falls through to the cross alignment - or to
		// `Stretch` when the cross axis is flexed at all, which is Roblox's
		// documented reading of `Automatic`.
		ItemLineAlignment LineAlignmentFor(
			const FlexItem *flex, const ListLayout &layout, FlexAlignment crossFlex, int crossAlign
		) {
			if (flex != nullptr && flex->ItemLine != ItemLineAlignment::Automatic) {
				return flex->ItemLine;
			}
			if (layout.ItemLine != ItemLineAlignment::Automatic) {
				return layout.ItemLine;
			}
			if (crossFlex != FlexAlignment::None) {
				return ItemLineAlignment::Stretch;
			}
			// The alignments and `ItemLineAlignment` agree on start/centre/end
			// order, one ordinal apart - `Automatic` occupies zero.
			return static_cast<ItemLineAlignment>(crossAlign + 1);
		}

		// The lead-in and the extra gap spare room becomes under one flex
		// spacing mode. `aligned` is where plain alignment would put the
		// block, which is the answer whenever no spacing mode applies.
		struct Spacing {
			float Lead = 0.0f;
			float Between = 0.0f;
		};

		Spacing SpacingFor(FlexAlignment flex, float spare, size_t count, float aligned) {
			if (spare <= 0.0f || count == 0) {
				return {aligned, 0.0f};
			}

			switch (flex) {
			case FlexAlignment::SpaceBetween:
				// One item has no between, and CSS parks it at the start.
				return count > 1 ? Spacing{0.0f, spare / static_cast<float>(count - 1)} : Spacing{};
			case FlexAlignment::SpaceAround: {
				const float share = spare / static_cast<float>(count);
				return {share * 0.5f, share};
			}
			case FlexAlignment::SpaceEvenly: {
				const float share = spare / static_cast<float>(count + 1);
				return {share, share};
			}
			case FlexAlignment::None:
			case FlexAlignment::Fill:
				break;
			}
			return {aligned, 0.0f};
		}

		// One line of a list, found by walking forward from `first`.
		//
		// Recomputed rather than stored: the walk below needs every line twice
		// - once to total the cross extents, once to place - and scanning a
		// range again is cheaper than a per-frame vector of lines at every
		// depth of the recursion, which is the allocation `ScratchAt` exists
		// to avoid.
		struct LineRun {
			// One past the last item on the line.
			size_t End = 0;

			// Basis sizes plus the authored gaps.
			float MainExtent = 0.0f;

			// The tallest basis cross size on the line.
			float CrossExtent = 0.0f;

			// The line's grow total, and its shrink total weighted by basis -
			// CSS's weighting, so a wide child gives up more than a narrow one.
			float Grow = 0.0f;
			float ShrinkWeight = 0.0f;
		};

		LineRun ScanLine(
			const std::vector<Item> &items,
			size_t first,
			bool horizontal,
			float gap,
			float mainSpan,
			bool wraps,
			FlexAlignment mainFlex
		) {
			LineRun line;
			size_t index = first;

			for (; index < items.size(); index++) {
				const float mainSize = horizontal ? items[index].Size.X : items[index].Size.Y;
				const float extended = line.MainExtent + (index > first ? gap : 0.0f) + mainSize;

				// The first item always lands, however big, or the walk would
				// never advance past a child wider than the container.
				if (wraps && index > first && extended > mainSpan + WRAP_TOLERANCE) {
					break;
				}

				line.MainExtent = extended;
				line.CrossExtent =
					std::max(line.CrossExtent, horizontal ? items[index].Size.Y : items[index].Size.X);

				const FlexFactors factors = FactorsFor(items[index].Found.Mods.Flex, mainFlex);
				line.Grow += factors.Grow;
				line.ShrinkWeight += factors.Shrink * mainSize;
			}

			line.End = index;
			return line;
		}

		// Places a container's children in a stack - one line, or several once
		// `Wraps` or a flex alignment is involved.
		size_t RunList(
			Store &store,
			const ListLayout &layout,
			std::vector<Item> &items,
			const Rect &area,
			const Rect &clip,
			int depth,
			float rotation
		) {
			Sort(items, layout.Order);

			const Vector2 span = area.Size();
			const bool horizontal = layout.Direction == FillDirection::Horizontal;
			const float mainSpan = horizontal ? span.X : span.Y;
			const float crossSpan = horizontal ? span.Y : span.X;

			// One `Padding` serves both axes, resolved against each: the gap
			// between items along a line, and between lines across them.
			const float gap = layout.Padding.Resolve(mainSpan);
			const float lineGap = layout.Padding.Resolve(crossSpan);

			// The flex pair is named by axis and the layout runs by role, so
			// which one is "along the fill" depends on the direction.
			const FlexAlignment mainFlex = horizontal ? layout.HorizontalFlex : layout.VerticalFlex;
			const FlexAlignment crossFlex = horizontal ? layout.VerticalFlex : layout.HorizontalFlex;
			const int mainAlign =
				horizontal ? static_cast<int>(layout.Horizontal) : static_cast<int>(layout.Vertical);
			const int crossAlign =
				horizontal ? static_cast<int>(layout.Vertical) : static_cast<int>(layout.Horizontal);

			// Pass one: every line, because the cross-axis arithmetic - where
			// the block of lines starts, and what `Fill` stretches each line by
			// - has to see all of them before the first is placed.
			size_t lineCount = 0;
			float linesExtent = 0.0f;
			for (size_t first = 0; first < items.size();) {
				const LineRun line =
					ScanLine(items, first, horizontal, gap, mainSpan, layout.Wraps, mainFlex);
				linesExtent += line.CrossExtent;
				lineCount++;
				first = line.End;
			}
			if (lineCount > 1) {
				linesExtent += lineGap * static_cast<float>(lineCount - 1);
			}

			// `Fill` across the lines is CSS's `align-content: stretch`: the
			// spare room grows every line equally and the block starts at the
			// top. The spacing members spend the same room between the lines.
			const float crossFree = crossSpan - linesExtent;
			float lineStretch = 0.0f;
			Spacing crossSpacing;
			if (crossFlex == FlexAlignment::Fill) {
				lineStretch =
					lineCount > 0 ? std::max(crossFree, 0.0f) / static_cast<float>(lineCount) : 0.0f;
			} else {
				crossSpacing = SpacingFor(
					crossFlex, crossFree, lineCount, AlignedStart(crossSpan, linesExtent, crossAlign)
				);
			}

			size_t placed = 0;
			float crossPos = (horizontal ? area.Min.Y : area.Min.X) + crossSpacing.Lead;

			for (size_t first = 0; first < items.size();) {
				const LineRun line =
					ScanLine(items, first, horizontal, gap, mainSpan, layout.Wraps, mainFlex);
				const float lineCross = line.CrossExtent + lineStretch;
				const size_t count = line.End - first;

				// Spare room feeds growth first; only what the growers leave is
				// spent as spacing. Shrinking is the same trade under overflow,
				// weighted by basis so a wide child gives up more - and a line
				// that cannot shrink overflows from its aligned start, exactly
				// as the single unwrapped line always has.
				const float freeRoom = mainSpan - line.MainExtent;
				float growUnit = 0.0f;
				float shrinkUnit = 0.0f;
				float effectiveExtent = line.MainExtent;
				if (freeRoom > 0.0f && line.Grow > 0.0f) {
					growUnit = freeRoom / line.Grow;
					effectiveExtent = mainSpan;
				} else if (freeRoom < 0.0f && line.ShrinkWeight > 0.0f) {
					shrinkUnit = freeRoom / line.ShrinkWeight;
					effectiveExtent = mainSpan;
				}

				const Spacing mainSpacing = SpacingFor(
					mainFlex,
					mainSpan - effectiveExtent,
					count,
					AlignedStart(mainSpan, effectiveExtent, mainAlign)
				);

				float along = (horizontal ? area.Min.X : area.Min.Y) + mainSpacing.Lead;

				for (size_t index = first; index < line.End; index++) {
					const Item &item = items[index];
					const FlexFactors factors = FactorsFor(item.Found.Mods.Flex, mainFlex);

					float mainSize = horizontal ? item.Size.X : item.Size.Y;
					mainSize += growUnit * factors.Grow + shrinkUnit * factors.Shrink * mainSize;

					// A ratio large enough to shrink past zero is clamped and
					// the loss is not redistributed - a second pass could hand
					// it to the survivors, and one pass that leaves a pixel
					// short beats two that can disagree.
					mainSize = std::max(mainSize, 0.0f);

					float crossSize = horizontal ? item.Size.Y : item.Size.X;
					const ItemLineAlignment lineAlign =
						LineAlignmentFor(item.Found.Mods.Flex, layout, crossFlex, crossAlign);

					float crossOffset = 0.0f;
					if (lineAlign == ItemLineAlignment::Stretch) {
						crossSize = lineCross;
					} else {
						// `Start`, `Center`, `End` are one past the alignment
						// ordinals `AlignedStart` reads.
						crossOffset = AlignedStart(lineCross, crossSize, static_cast<int>(lineAlign) - 1);
					}

					const Vector2 size =
						horizontal ? Vector2{mainSize, crossSize} : Vector2{crossSize, mainSize};
					const Vector2 corner = horizontal ? Vector2{along, crossPos + crossOffset}
													  : Vector2{crossPos + crossOffset, along};

					placed +=
						Place(store, item.Node, FromCorner(corner, size), clip, depth, rotation, item.Found);
					along += mainSize + gap + mainSpacing.Between;
				}

				crossPos += lineCross + lineGap + crossSpacing.Between;
				first = line.End;
			}

			return placed;
		}

		// Places a container's children on a grid.
		size_t RunGrid(
			Store &store,
			const GridLayout &layout,
			std::vector<Item> &items,
			const Rect &area,
			const Rect &clip,
			int depth,
			float rotation
		) {
			Sort(items, layout.Order);

			const Vector2 span = area.Size();
			const Vector2 cell = layout.CellSize.Resolve(span);
			const Vector2 gap = layout.CellPadding.Resolve(span);

			// **The cell's own size is what the child gets, not what it asked
			// for.** That is the difference between a grid and a list: a grid
			// decides both axes, which is why `Measure` is not consulted here.
			const bool horizontal = layout.Direction == FillDirection::Horizontal;
			const float track = horizontal ? span.X : span.Y;
			const float step = (horizontal ? cell.X : cell.Y) + (horizontal ? gap.X : gap.Y);

			const int32_t perLine =
				CellsPerLine(layout, track, horizontal ? cell.X : cell.Y, horizontal ? gap.X : gap.Y);

			const bool fromRight =
				layout.Corner == StartCorner::TopRight || layout.Corner == StartCorner::BottomRight;
			const bool fromBottom =
				layout.Corner == StartCorner::BottomLeft || layout.Corner == StartCorner::BottomRight;

			const auto lines = static_cast<int32_t>(
				(items.size() + static_cast<size_t>(perLine) - 1) / static_cast<size_t>(perLine)
			);

			const float usedAcross = horizontal ? static_cast<float>(lines) * (cell.Y + gap.Y) - gap.Y
												: static_cast<float>(lines) * (cell.X + gap.X) - gap.X;

			const float alignAcross =
				horizontal ? AlignedStart(span.Y, usedAcross, static_cast<int>(layout.Vertical))
						   : AlignedStart(span.X, usedAcross, static_cast<int>(layout.Horizontal));

			size_t placed = 0;
			for (size_t index = 0; index < items.size(); index++) {
				const auto line = static_cast<int32_t>(index) / perLine;
				auto slot = static_cast<int32_t>(index) % perLine;

				// How many are on *this* line, so a short final line aligns
				// against its own width rather than against a full one.
				const auto onLine = static_cast<int32_t>(std::min<size_t>(
					static_cast<size_t>(perLine),
					items.size() - static_cast<size_t>(line) * static_cast<size_t>(perLine)
				));

				const float usedAlong = static_cast<float>(onLine) * step - (horizontal ? gap.X : gap.Y);
				const float alignAlong =
					horizontal ? AlignedStart(span.X, usedAlong, static_cast<int>(layout.Horizontal))
							   : AlignedStart(span.Y, usedAlong, static_cast<int>(layout.Vertical));

				// The along axis is X for a horizontal fill and Y for a
				// vertical one, so which corner flag reverses which index
				// depends on the direction. Written out rather than folded into
				// one expression, because the folded version was wrong and read
				// as correct.
				if (horizontal ? fromRight : fromBottom) {
					slot = onLine - 1 - slot;
				}

				auto row = line;
				if (horizontal ? fromBottom : fromRight) {
					row = lines - 1 - line;
				}

				const float alongOffset = alignAlong + static_cast<float>(slot) * step;
				const float acrossOffset =
					alignAcross + static_cast<float>(row) * ((horizontal ? cell.Y + gap.Y : cell.X + gap.X));

				const Vector2 corner = horizontal
										   ? Vector2{area.Min.X + alongOffset, area.Min.Y + acrossOffset}
										   : Vector2{area.Min.X + acrossOffset, area.Min.Y + alongOffset};

				placed += Place(
					store,
					items[index].Node,
					FromCorner(corner, cell),
					clip,
					depth,
					rotation,
					items[index].Found
				);
			}

			return placed;
		}

		// Lays the parent's children out as rows and their children as cells.
		//
		// **The only layout here that reaches two levels down**, because a
		// column has to be one width in every row or the thing is not a table.
		// `gui::TableLayout` carries the argument; what follows is its two
		// consequences.
		//
		// **A row's `Resolved` is written here rather than through `Place`**, and
		// that is not a shortcut: `Place` would go on to lay the row's children
		// out by *their* own rules, and this function is about to place them by
		// the table's. Running both would place every cell twice and leave the
		// arena's per-level scratch being written by two levels at once.
		//
		// **The cell lists are local vectors rather than the per-level scratch**,
		// for the same collision: `Place` on a cell uses the scratch at the depth
		// this function would want. A table is a handful of rows, so an
		// allocation per row per rebuild is a cost nobody will find - and a
		// rebuild is not a frame.
		size_t RunTable(
			Store &store,
			const TableLayout &layout,
			std::vector<Item> &rows,
			const Rect &area,
			const Rect &clip,
			int depth,
			float rotation,
			std::vector<Entity> &arena
		) {
			Sort(rows, layout.Order);

			const bool stacked = layout.Direction == FillDirection::Vertical;
			const Vector2 span = area.Size();
			const Vector2 gap = layout.Padding.Resolve(span);
			const bool named = layout.Order == SortOrder::Name;

			// Along a row and across the rows. For a horizontal table the two
			// swap, which is the whole of what `FillDirection` does here.
			const auto along = [&](const Vector2 &value) { return stacked ? value.X : value.Y; };
			const auto across = [&](const Vector2 &value) { return stacked ? value.Y : value.X; };

			std::vector<std::vector<Item>> table;
			table.reserve(rows.size());

			std::vector<float> cellExtent;
			std::vector<float> lineExtent;
			lineExtent.reserve(rows.size());

			for (const Item &row : rows) {
				std::vector<Item> cells;
				ChildItems(store, row.Found, row.Size, depth + 1, named, cells, arena);
				Sort(cells, layout.Order);

				float line = across(row.Size);
				for (size_t index = 0; index < cells.size(); index++) {
					if (index >= cellExtent.size()) {
						cellExtent.push_back(0.0f);
					}
					cellExtent[index] = std::max(cellExtent[index], along(cells[index].Size));
					line = std::max(line, across(cells[index].Size));
				}

				lineExtent.push_back(line);
				table.push_back(std::move(cells));
			}

			const auto total = [](const std::vector<float> &extents, float between) {
				float sum = 0.0f;
				for (const float extent : extents) {
					sum += extent;
				}
				return extents.empty() ? 0.0f : sum + between * static_cast<float>(extents.size() - 1);
			};

			// **Spare room is shared evenly, not proportionally.** Roblox fills
			// the empty space; an even share is the reading under which two
			// columns of very different content end up the same width, which is
			// what a table with a header row looks like.
			const auto fill = [](std::vector<float> &extents, float slack) {
				if (extents.empty() || !(slack > 0.0f)) {
					return;
				}
				const float each = slack / static_cast<float>(extents.size());
				for (float &extent : extents) {
					extent += each;
				}
			};

			if (layout.FillEmptySpaceColumns) {
				fill(cellExtent, along(span) - total(cellExtent, along(gap)));
			}
			if (layout.FillEmptySpaceRows) {
				fill(lineExtent, across(span) - total(lineExtent, across(gap)));
			}

			const float usedAlong = total(cellExtent, along(gap));
			const float usedAcross = total(lineExtent, across(gap));

			const float startAlong = AlignedStart(
				along(span),
				usedAlong,
				stacked ? static_cast<int>(layout.Horizontal) : static_cast<int>(layout.Vertical)
			);
			const float startAcross = AlignedStart(
				across(span),
				usedAcross,
				stacked ? static_cast<int>(layout.Vertical) : static_cast<int>(layout.Horizontal)
			);

			const auto corner = [&](float alongAt, float acrossAt) {
				return stacked ? Vector2{area.Min.X + alongAt, area.Min.Y + acrossAt}
							   : Vector2{area.Min.X + acrossAt, area.Min.Y + alongAt};
			};
			const auto extent = [&](float alongAt, float acrossAt) {
				return stacked ? Vector2{alongAt, acrossAt} : Vector2{acrossAt, alongAt};
			};

			size_t placed = 0;
			float acrossAt = startAcross;

			for (size_t line = 0; line < rows.size(); line++) {
				const Rect rowRect =
					FromCorner(corner(startAlong, acrossAt), extent(usedAlong, lineExtent[line]));

				Resolved value;
				value.AbsolutePosition = rowRect.Min;
				value.AbsoluteSize = rowRect.Size();
				value.AbsoluteRotation = rotation;
				value.Clip = clip;
				value.Depth = depth;
				value.Rendered = true;
				store.Set(rows[line].Node, value);
				placed++;

				const Element *rowElement = store.Get<Element>(rows[line].Node);
				const Rect inner =
					rowElement != nullptr && rowElement->ClipsDescendants ? clip.Intersection(rowRect) : clip;

				float alongAt = startAlong;
				for (size_t column = 0; column < table[line].size(); column++) {
					const Rect cellRect =
						FromCorner(corner(alongAt, acrossAt), extent(cellExtent[column], lineExtent[line]));
					placed += Place(
						store,
						table[line][column].Node,
						cellRect,
						inner,
						depth + 1,
						rotation,
						table[line][column].Found
					);
					alongAt += cellExtent[column] + along(gap);
				}

				acrossAt += lineExtent[line] + across(gap);
			}

			return placed;
		}

		// How hard the rubber band pulls back, as a time constant in seconds.
		//
		// **An exponential decay rather than a spring integration.** A spring
		// needs a velocity and a step; a decay is a closed form, which is what
		// makes the recovery a function of elapsed time - frame-rate
		// independent by construction, and a thing a suite states rather than
		// steps a hundred frames to reach.
		constexpr float ELASTIC_SPRING_SECONDS = 0.12f;

		// Below this the canvas is back and the state is dropped, so a settled
		// frame stops folding a new number every tick.
		constexpr float ELASTIC_SETTLED_PIXELS = 0.5f;

		// Resolves every scrolling frame's rubber band against the clock, once.
		//
		// Held: the canvas sits where the drag put it. Let go: it comes back
		// along a decay whose zero is the end of the canvas.
		//
		// @param store   The world.
		// @param seconds The caller's clock. Only differences are read.
		void AdvanceScrolling(Store &store, double seconds, Entity collector = ecs::NULL_ENTITY) {
			std::vector<Entity> frames;
			store.Each<const ScrollMotion>([&](Entity entity, const ScrollMotion &) {
				if (collector == ecs::NULL_ENTITY || store.IsDescendantOf(entity, collector)) {
					frames.push_back(entity);
				}
			});

			for (const Entity entity : frames) {
				const ScrollMotion *had = store.Get<ScrollMotion>(entity);
				if (had == nullptr) {
					continue;
				}
				ScrollMotion motion = *had;

				if (motion.Held) {
					// A held canvas is wherever the hand put it, and the spring
					// has not been let go of yet.
					motion.Overshoot = motion.Pull;
					motion.ReleasedAt = -1.0;
					store.Set(entity, motion);
					continue;
				}

				// **Stamped here rather than by the router**, which is what
				// keeps the clock in one place. The router says "this was let
				// go of"; the first layout after that says when.
				if (motion.ReleasedAt < 0.0) {
					motion.Released = motion.Pull;
					motion.Pull = core::Vector2{};
					motion.ReleasedAt = seconds;
				}

				const auto since = static_cast<float>(seconds - motion.ReleasedAt);
				const float decay = std::exp(-std::max(since, 0.0f) / ELASTIC_SPRING_SECONDS);
				motion.Overshoot = core::Vector2{motion.Released.X * decay, motion.Released.Y * decay};

				if (std::abs(motion.Overshoot.X) < ELASTIC_SETTLED_PIXELS &&
					std::abs(motion.Overshoot.Y) < ELASTIC_SETTLED_PIXELS) {
					// Settled. Everything back to rest, so the fold below stops
					// moving and the list stops rebuilding.
					motion = ScrollMotion{};
				}

				store.Set(entity, motion);
			}
		}

		// Resolves every page layout's slide against the clock, once.
		//
		// **Before the walk rather than inside it, and that is the whole design
		// of this feature.** `Place` is recursive and reached from five run
		// functions; threading a clock through all of them to serve one caller
		// would put a time argument on every layout in the module. One pass
		// over a packed column beforehand leaves the recursion exactly as
		// clock-free as it was, and leaves `Alpha` - a number - as the only
		// thing `RunPages` has to read.
		//
		// @param store   The world.
		// @param seconds The caller's clock. Only differences are read.
		void AdvancePages(Store &store, double seconds, Entity collector = ecs::NULL_ENTITY) {
			// **Collected before anything is written**, for the reason the
			// collector walk gives: `store.Set` can move a row between
			// archetypes, and moving one out from under the query walking it is
			// what `Store::Each`'s deferral exists to prevent.
			std::vector<Entity> layouts;
			store.Each<const PageLayout>([&](Entity entity, const PageLayout &) {
				if (collector == ecs::NULL_ENTITY || store.IsDescendantOf(entity, collector)) {
					layouts.push_back(entity);
				}
			});

			for (const Entity entity : layouts) {
				const PageLayout *layout = store.Get<PageLayout>(entity);
				if (layout == nullptr) {
					continue;
				}

				PageMotion motion;
				if (const PageMotion *had = store.Get<PageMotion>(entity)) {
					motion = *had;
				}

				// **The first sight of a layout adopts its page rather than
				// sliding to it**, and that distinction is the whole difference
				// between an interface that opens and one that swings into
				// place every time it is shown. A `UIPageLayout` authored with
				// `CurrentPage` already set is describing where it *starts*; a
				// script assigning the same property later is asking for a
				// jump, and only the second is motion.
				//
				// `StartedAt` is the marker, which is why it is written here
				// even when nothing moved: negative means this layout has never
				// been resolved.
				if (motion.StartedAt < 0.0) {
					motion.From = layout->CurrentPage;
					motion.To = layout->CurrentPage;
					motion.StartedAt = seconds;
					motion.Alpha = 1.0f;
				} else if (motion.To != layout->CurrentPage) {
					// **A jump is noticed rather than announced.** There is no
					// `JumpTo` to hook and no setter this module owns - the
					// class table writes `CurrentPage` straight into the
					// component - so the difference between what is wanted and
					// what was wanted last time is the event.
					motion.From = motion.To;
					motion.To = layout->CurrentPage;
					motion.StartedAt = seconds;
					motion.Alpha = 0.0f;
				}

				const bool moving = motion.From != motion.To && motion.StartedAt >= 0.0;
				const bool tweens = moving && layout->Animated && layout->TweenTime > 0.0f;

				if (!moving) {
					motion.Alpha = 1.0f;
				} else if (!tweens) {
					// **`Animated` off and a zero `TweenTime` both cut**, and
					// they are still two properties: one says this layout does
					// not animate and the other says it animates over no time.
					motion.From = motion.To;
					motion.Alpha = 1.0f;
				} else {
					const double elapsed = seconds - motion.StartedAt;
					const float through = static_cast<float>(elapsed / layout->TweenTime);
					motion.Alpha = core::TweenInfo::Ease(through, layout->Easing, layout->EasingWay);

					// **Settled by collapsing `From` onto `To`**, which is what
					// stops the signature moving once the slide is over. A
					// layout left "animating at alpha 1" would fold a different
					// number every frame and rebuild the whole list forever.
					if (through >= 1.0f) {
						motion.From = motion.To;
						motion.Alpha = 1.0f;
					}
				}

				store.Set(entity, motion);
			}
		}

		// Shows one of the parent's children and slides the rest aside.
		//
		// Every page is the parent's own size and sits one step further along
		// than the one before it, so the strip moves under the container rather
		// than the pages moving inside it.
		//
		// **The slide is a fractional page index rather than an integer one.**
		// `AdvancePages` has already turned the clock into `PageMotion::Alpha`,
		// so all this does is put the strip between two pages instead of on one
		// - which is why nothing here reads a time and why an overshooting
		// curve needs no special case.
		size_t RunPages(
			Store &store,
			Entity owner,
			const PageLayout &layout,
			std::vector<Item> &items,
			const Rect &area,
			const Rect &clip,
			int depth,
			float rotation
		) {
			Sort(items, layout.Order);

			const Vector2 span = area.Size();
			const bool horizontal = layout.Direction == FillDirection::Horizontal;
			const float step =
				(horizontal ? span.X : span.Y) + layout.Padding.Resolve(horizontal ? span.X : span.Y);

			// Which page is under the container. An unset or stale `CurrentPage`
			// is the first one, which is what an author who set nothing means and
			// what a page destroyed while it was showing leaves behind.
			const auto indexOf = [&items](Entity page) {
				for (size_t index = 0; index < items.size(); index++) {
					if (items[index].Node == page) {
						return static_cast<float>(index);
					}
				}
				return 0.0f;
			};

			const auto count = static_cast<int32_t>(items.size());

			// **Wrapping is applied to the *distance* as well as to each page's
			// offset**, and it has to be: a three-page carousel jumping from the
			// last page to the first should slide one step forward, not two
			// steps back past everything. Without this the loop reads as a
			// rewind, which is the one thing `Circular` exists to prevent.
			const auto shortest = [&](float from, float to) {
				float delta = to - from;
				if (layout.Circular && count > 0) {
					const auto whole = static_cast<float>(count);
					if (delta > whole * 0.5f) {
						delta -= whole;
					} else if (delta < -whole * 0.5f) {
						delta += whole;
					}
				}
				return delta;
			};

			float current = indexOf(layout.CurrentPage);
			if (const PageMotion *motion = store.Get<PageMotion>(owner);
				motion != nullptr && motion->From != motion->To) {
				const float from = indexOf(motion->From);
				current = from + shortest(from, indexOf(motion->To)) * motion->Alpha;
			}

			size_t placed = 0;

			for (size_t index = 0; index < items.size(); index++) {
				float offset = static_cast<float>(index) - current;

				// **Wrapped to the nearer side**, which is what makes a circular
				// strip read as a loop: the page before the first is drawn just
				// off the near edge rather than at the far end of everything.
				if (layout.Circular && count > 0) {
					const auto whole = static_cast<float>(count);
					if (offset > whole * 0.5f) {
						offset -= whole;
					} else if (offset < -whole * 0.5f) {
						offset += whole;
					}
				}

				const float slide = offset * step;
				const Vector2 corner = horizontal ? Vector2{area.Min.X + slide, area.Min.Y}
												  : Vector2{area.Min.X, area.Min.Y + slide};

				placed += Place(
					store,
					items[index].Node,
					FromCorner(corner, span),
					clip,
					depth,
					rotation,
					items[index].Found
				);
			}

			return placed;
		}

		// Writes one node's `Resolved` and descends.
		//
		// `rect` is where this node goes and is already final - a list layout
		// decided it, or the caller resolved the node's own `UDim2`. Splitting
		// the decision out is what lets one function serve both.
		// @param known The node's modifiers when the caller already walked for
		//        them - which every recursive caller has, because measuring a
		//        child is what produced its size. Null at the roots, where
		//        nothing has measured anything yet.
		size_t Place(
			Store &store,
			Entity instance,
			const Rect &rect,
			const Rect &clip,
			int depth,
			float rotation,
			const Scan &scan
		) {
			if (depth > MAXIMUM_DEPTH) {
				// `Store::SetParent` refuses a cycle, so reaching this means
				// something built a tree deeper than any interface is, or built
				// one another way. Either is a subtree that stops being placed
				// while everything above it keeps drawing.
				ENGINE_WARN_EVERY(
					5.0,
					"layout stopped at depth {} under entity {}; the subtree is not placed",
					depth,
					instance.Id
				);
				return 0;
			}

			const Element *element = store.Get<Element>(instance);
			if (element == nullptr) {
				return 0;
			}

			// Not marked unrendered here - every `Resolved` was cleared before
			// the walk began, so a subtree the walk does not enter is already
			// saying it is not on screen. That is one pass over a packed column
			// instead of a recursion per hidden node, and it cannot miss a
			// branch the way a hook on every parenting path can.
			if (!element->Visible) {
				return 0;
			}

			const Modifiers &modifiers = scan.Mods;

			const float total = rotation + element->Rotation;

			Resolved value;
			value.AbsolutePosition = rect.Min;
			value.AbsoluteSize = rect.Size();
			value.AbsoluteRotation = total;
			value.Clip = clip;
			value.Depth = depth;
			value.Rendered = true;

			if (const Label *label = store.Get<Label>(instance)) {
				value.TextSize = FittedTextSize(
					*label,
					rect.Size(),
					modifiers.TextLimits,
					modifiers.Factor != nullptr ? modifiers.Factor->Factor : 1.0f
				);

				// **The same estimate the fit used, kept rather than recomputed
				// by whoever asks.** `TextBounds` and `TextFits` are two readings
				// of one measurement, and a script that derived the second from
				// the first would be doing arithmetic this pass has already done
				// with the numbers it already had.
				const auto characters = static_cast<float>(DrawnCharacters(*label));
				const auto drawn = static_cast<float>(value.TextSize);
				const auto lines = static_cast<float>(TextLines(label->Text));
				value.TextBounds = Vector2{
					characters * AVERAGE_ADVANCE * drawn,
					lines * LINE_SPACING * drawn,
				};
				value.TextFits = value.TextBounds.X <= rect.Width() && value.TextBounds.Y <= rect.Height();
			}

			store.Set(instance, value);

			// **The clip is intersected here and inherited downwards**, so a
			// node clipped by three ancestors costs three intersections in
			// total rather than three per descendant.
			const Rect inner = element->ClipsDescendants ? clip.Intersection(rect) : clip;

			// **Marked before the children are measured and released after they
			// are placed.** Measuring each child appends that child's own
			// children, and every one of those runs has to survive until its
			// owner is placed - which is after all of its siblings were measured
			// on top of it. Releasing to the mark at the end drops the whole
			// generation at once, so the arena's high-water mark is the widest
			// path through the tree rather than the tree.
			//
			// **Opened before the content area rather than after it**, because
			// `AutomaticCanvas` measures the children to size the canvas and that
			// measure appends to the same arena.
			std::vector<Entity> &arena = ChildArena();
			const ArenaScope scope(arena);

			Rect area;
			ScrollState scrolled;
			if (ContentArea(store, instance, rect, modifiers, scan, depth, arena, area, scrolled)) {
				store.Set(instance, scrolled);
			}

			size_t placed = 1;

			// This level's scratch. Live only until the loops below finish with
			// it: every recursive call uses the next level's, and by the time
			// this one returns nothing is reading it.
			std::vector<Item> &items = ScratchAt(depth);

			ChildItems(store, scan, area.Size(), depth, SortsByName(modifiers), items, arena);

			// **One layout wins and the order is the declaration order.** Two on
			// one container is an authoring mistake with no sensible reading -
			// there is no arrangement that is both a grid and a page strip - so
			// the first found decides rather than the two fighting per frame.
			// Which of the four won is invisible afterwards, and an author who
			// added the second one sees the first one's arrangement with nothing
			// mentioning the one that was ignored.
			if (const int arrangements = (modifiers.List != nullptr) + (modifiers.Grid != nullptr) +
										 (modifiers.Table != nullptr) + (modifiers.Page != nullptr);
				arrangements > 1) {
				ENGINE_WARN_EVERY(
					5.0,
					"entity {} carries {} layout modifiers; {} wins and the rest are ignored",
					instance.Id,
					arrangements,
					modifiers.List != nullptr	 ? "list"
					: modifiers.Grid != nullptr	 ? "grid"
					: modifiers.Table != nullptr ? "table"
												 : "page"
				);
			}

			if (modifiers.List != nullptr) {
				placed += RunList(store, *modifiers.List, items, area, inner, depth + 1, total);
			} else if (modifiers.Grid != nullptr) {
				placed += RunGrid(store, *modifiers.Grid, items, area, inner, depth + 1, total);
			} else if (modifiers.Table != nullptr) {
				placed += RunTable(store, *modifiers.Table, items, area, inner, depth + 1, total, arena);
			} else if (modifiers.Page != nullptr) {
				placed +=
					RunPages(store, modifiers.PageOn, *modifiers.Page, items, area, inner, depth + 1, total);
			} else {
				for (const Item &item : items) {
					const Element *child = store.Get<Element>(item.Node);
					const Vector2 anchored = child->Position.Resolve(area.Size());
					const Vector2 corner{
						area.Min.X + anchored.X - child->AnchorPoint.X * item.Size.X,
						area.Min.Y + anchored.Y - child->AnchorPoint.Y * item.Size.Y,
					};
					placed += Place(
						store, item.Node, FromCorner(corner, item.Size), inner, depth + 1, total, item.Found
					);
				}
			}

			return placed;
		}

		// How far a collector that clips nothing lets its subtree reach.
		//
		// **A number rather than an infinity**, because the clip is intersected
		// on the way down and a `Rect` holding infinities produces a NaN the
		// first time one is subtracted from another. A canvas is measured in
		// pixels and no interface is a million of them across in either
		// direction, so this cuts nothing an author meant to draw and stays
		// arithmetic every reader can do.
		constexpr float UNCLIPPED_REACH = 1.0e6f;

		// The rectangle one collector's roots lay out inside.
		//
		// @param clip Filled with the rectangle the subtree is cut to, which is
		//        `out` for a collector that clips and a very large rectangle for
		//        one that does not. **Two outputs rather than one**, because a
		//        `SurfaceGui` with `ClipsDescendants` off still lays its children
		//        out against the canvas and merely draws them past its edge - the
		//        two rectangles answer different questions and folding them into
		//        one would make an unclipped surface resolve every `UDim2` scale
		//        against a million pixels.
		// @return `false` for a collector that has no world-owned canvas. A
		//         `PluginGui` instead enters through `LayoutCollector` with the
		//         rectangle its host chose.
		bool CanvasFor(const Store &store, Entity collector, const Screen &screen, Rect &out, Rect &clip) {
			const LayoutIds &ids = LayoutClasses();

			const auto unclipped = [](const Rect &canvas) {
				const Vector2 centre{
					(canvas.Min.X + canvas.Max.X) * 0.5f,
					(canvas.Min.Y + canvas.Max.Y) * 0.5f,
				};
				return Rect{
					Vector2{centre.X - UNCLIPPED_REACH, centre.Y - UNCLIPPED_REACH},
					Vector2{centre.X + UNCLIPPED_REACH, centre.Y + UNCLIPPED_REACH},
				};
			};

			if (store.IsA(collector, ids.ScreenGui)) {
				const Layer *layer = store.Get<Layer>(collector);
				const float top = layer != nullptr && layer->IgnoreGuiInset ? 0.0f : screen.TopInset;
				out = Rect{Vector2{0.0f, top}, Vector2{screen.Width, screen.Height}};

				// **A screen gui always clips and has no property saying so**,
				// which is Roblox's shape: the canvas *is* the display, so there
				// is nowhere past it for anything to be drawn.
				clip = out;
				return true;
			}

			// **What a host resolved wins, and its absence is not a failure.**
			// `SpatialCanvas` is written by whoever holds the camera and the
			// world - the multiplication `D00022` said belongs to whoever has
			// both operands - and a world nobody is drawing simply has none. The
			// authored fallbacks below are then the answer rather than a
			// degraded one: a `SurfaceGui` in `FixedSize` mode wanted its pixel
			// size all along, and a headless test asserting a layout wants a
			// number that does not depend on where a camera happens to be.
			const SpatialCanvas *resolved = store.Get<SpatialCanvas>(collector);
			if (resolved != nullptr && !resolved->Visible) {
				return false;
			}

			if (const Surface *surface = store.Get<Surface>(collector);
				surface != nullptr && store.IsA(collector, ids.SurfaceGui)) {
				out = Rect{Vector2::Zero, resolved != nullptr ? resolved->Size : surface->CanvasSize};
				clip = surface->ClipsDescendants ? out : unclipped(out);
				return true;
			}

			if (const Billboard *billboard = store.Get<Billboard>(collector);
				billboard != nullptr && store.IsA(collector, ids.BillboardGui)) {
				// Offset only when nothing resolved one - a billboard's scale is
				// in studs against the screen it is projected onto, so without a
				// camera there is no number to multiply it by.
				out = Rect{
					Vector2::Zero,
					resolved != nullptr ? resolved->Size
										: Vector2{billboard->Size.X.Offset, billboard->Size.Y.Offset},
				};
				clip = billboard->ClipsDescendants ? out : unclipped(out);
				return true;
			}

			return false;
		}

		size_t PlaceCollector(Store &store, Entity collector, const Rect &canvas, const Rect &clip) {
			store.Set(collector, Canvas{canvas});

			Resolved collectorResolved;
			collectorResolved.AbsolutePosition = canvas.Min;
			collectorResolved.AbsoluteSize = canvas.Size();
			collectorResolved.Clip = clip;
			collectorResolved.Rendered = true;
			store.Set(collector, collectorResolved);

			std::vector<Entity> roots;
			store.EachChild(collector, [&](Entity child) { roots.push_back(child); });

			size_t placed = 0;
			for (const Entity root : roots) {
				const Element *element = store.Get<Element>(root);
				if (element == nullptr) {
					continue;
				}

				// The root owns one arena scope. Every recursive placement releases
				// its own run, so this mark prevents roots accumulating between passes.
				std::vector<Entity> &arena = ChildArena();
				const ArenaScope scope(arena);

				Scan scan;
				const Vector2 size = Measure(store, root, canvas.Size(), 1, scan, arena);
				const Vector2 anchored = element->Position.Resolve(canvas.Size());
				const Vector2 corner{
					canvas.Min.X + anchored.X - element->AnchorPoint.X * size.X,
					canvas.Min.Y + anchored.Y - element->AnchorPoint.Y * size.Y,
				};
				placed += Place(store, root, FromCorner(corner, size), clip, 1, 0.0f, scan);
			}
			return placed;
		}

		void ReportPlaced(size_t placed) {
			if (placed == 0) {
				return;
			}
			core::Metrics::Count("gui.layout.placed", static_cast<double>(placed));
			ENGINE_TRACE("laid out {} element(s)", placed);
		}
	}

	size_t Layout(Store &store, const Screen &screen, double seconds) {
		ENGINE_PROFILE_CAT("gui layout", engine::core::ProfileCategory::ECS);

		// Forces the class table up before the first `IsA` below, which is
		// what makes a store that has never seen this module still lay out.
		LayoutClasses();

		// **The only two places in this module that read the clock, and both
		// are here rather than in the walk.** Each turns elapsed seconds into a
		// number - a page's `Alpha`, a canvas's spring - so everything below
		// places rectangles exactly as it did before any of this existed.
		AdvancePages(store, seconds);
		AdvanceScrolling(store, seconds);

		// **Cleared first, then set by the walk.** Anything the walk does not
		// reach is an orphan - an element a script created and has not
		// parented, one under a `Part`, one beneath a disabled collector or an
		// invisible ancestor - and Roblox draws none of those.
		//
		// A sweep rather than a hook on every parenting path, which is
		// `scene::Visibility`'s argument in as many words: ancestry is not
		// local, and the path that gets missed is the one where an element
		// draws after being detached. One pass over a packed column, against a
		// recursion per hidden subtree.
		//
		// **Only the flag is cleared.** The rectangles stay, so an element
		// scrolled out of view is distinguishable from one never laid out -
		// `Resolved`'s own comment gives the reason.
		store.Each<Resolved>([](Entity, Resolved &resolved) { resolved.Rendered = false; });

		// **Collected before anything is written.** `Place` writes `Resolved`,
		// which can move a row between archetypes the first time a node gets
		// one - and moving a row out from under the query walking it is exactly
		// what `Store::Each`'s deferral exists to prevent. A vector of handles
		// costs one allocation per frame against a class of bug that only
		// appears once a UI is big enough to span two archetypes.
		std::vector<Entity> collectors;
		store.Each<const Layer>([&](Entity entity, const Layer &) { collectors.push_back(entity); });

		// Draw order, and stable so two collectors sharing a `DisplayOrder`
		// keep the order the store held them in rather than swapping between
		// frames as archetypes reshuffle.
		std::stable_sort(collectors.begin(), collectors.end(), [&](Entity left, Entity right) {
			const Layer *a = store.Get<Layer>(left);
			const Layer *b = store.Get<Layer>(right);
			return (a != nullptr ? a->DisplayOrder : 0) < (b != nullptr ? b->DisplayOrder : 0);
		});

		size_t placed = 0;

		const LayoutIds &ids = LayoutClasses();

		for (const Entity collector : collectors) {
			const Layer *layer = store.Get<Layer>(collector);
			Rect canvas;
			Rect clip;

			// **Where it sits decides whether it draws at all**, before
			// anything asks how big it is. A `SurfaceGui` or a `BillboardGui`
			// hangs off something in the world, so `Workspace` is a legal home
			// for it; a `ScreenGui` is the viewer's own overlay and is not in
			// the world at all, so it is not.
			const bool spatial =
				store.IsA(collector, ids.SurfaceGui) || store.IsA(collector, ids.BillboardGui);

			const bool drawn = layer != nullptr && layer->Enabled && Contained(store, collector, spatial) &&
							   CanvasFor(store, collector, screen, canvas, clip);

			if (!drawn) {
				continue;
			}

			placed += PlaceCollector(store, collector, canvas, clip);
		}

		// Per layout pass rather than per frame: `Compiled::Rebuild` only calls
		// this when the signature moved, so the rate reads as work done rather
		// than as a number multiplied by the frame rate.
		//
		// **Nothing at all when nothing was placed**, because a client with no
		// interface lays out zero elements on every frame and neither the
		// counter's lock nor the line is worth paying for that.
		ReportPlaced(placed);
		return placed;
	}

	size_t LayoutCollector(Store &store, Entity collector, const Screen &screen, double seconds) {
		ENGINE_PROFILE_CAT("gui collector layout", engine::core::ProfileCategory::ECS);

		const LayoutIds &ids = LayoutClasses();
		if (!store.Alive(collector) || !store.IsA(collector, ids.Collector)) {
			return 0;
		}

		AdvancePages(store, seconds, collector);
		AdvanceScrolling(store, seconds, collector);

		if (Resolved *resolved = store.GetMutable<Resolved>(collector); resolved != nullptr) {
			resolved->Rendered = false;
		}
		store.EachDescendant(collector, [&](Entity descendant) {
			if (Resolved *resolved = store.GetMutable<Resolved>(descendant); resolved != nullptr) {
				resolved->Rendered = false;
			}
		});

		const Layer *layer = store.Get<Layer>(collector);
		if (layer == nullptr || !layer->Enabled || screen.Width <= 0.0f || screen.Height <= 0.0f) {
			return 0;
		}

		const Rect canvas{Vector2::Zero, Vector2{screen.Width, screen.Height}};
		const size_t placed = PlaceCollector(store, collector, canvas, canvas);
		ReportPlaced(placed);
		return placed;
	}
}
