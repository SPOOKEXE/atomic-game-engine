#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>

#include <algorithm>
#include <cmath>
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
		// **Ids and not names.** `Store::IsA` is an ancestor scan over a handful
		// of integers; building a `core::Name` per node per frame would take the
		// process-wide registry's mutex once per element, which is the exact
		// cost `scene`'s `NormalIdEnum` comment measures.
		struct Ids {
			ClassId Object;
			ClassId Collector;
			ClassId ScreenGui;
			ClassId SurfaceGui;
			ClassId BillboardGui;

			Ids() {
				RegisterGuiClasses();
				Object = GuiClass("GuiObject");
				Collector = GuiClass("LayerCollector");
				ScreenGui = GuiClass("ScreenGui");
				SurfaceGui = GuiClass("SurfaceGui");
				BillboardGui = GuiClass("BillboardGui");
			}
		};

		const Ids &Classes() {
			static const Ids ids;
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
		struct Containers {
			core::Name Workspace{WORKSPACE};
			core::Name StarterGui{STARTER_GUI};
			core::Name PlayerGui{PLAYER_GUI};
		};

		const Containers &Roots() {
			static const Containers names;
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
			const Containers &roots = Roots();

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

		// The rectangle a container's children are placed inside.
		//
		// The container's own rectangle, less its padding, less any scroll
		// offset. A scrolling frame's canvas is larger than the frame and moves
		// under it, which is the whole of what scrolling is here.
		Rect ContentArea(const Store &store, Entity instance, const Rect &own, const Modifiers &modifiers) {
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

			if (const Scrolling *scrolling = store.Get<Scrolling>(instance)) {
				// The canvas resolves against the *padded* frame, so a padded
				// scrolling frame does not scroll its padding away.
				const Vector2 canvas = scrolling->CanvasSize.Resolve(size);
				origin =
					Vector2{origin.X - scrolling->CanvasPosition.X, origin.Y - scrolling->CanvasPosition.Y};
				size = Vector2{std::max(canvas.X, size.X), std::max(canvas.Y, size.Y)};
			}

			return FromCorner(origin, size);
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
		int32_t FittedTextSize(const Label &label, const Vector2 &box, const TextSizeLimits *limits) {
			int32_t size = label.Size;

			if (label.Scaled) {
				const size_t characters = std::max<size_t>(label.Text.size(), 1);
				const float byWidth = box.X / (static_cast<float>(characters) * AVERAGE_ADVANCE);
				const float byHeight = box.Y / LINE_SPACING;

				// Floored rather than rounded. A size that rounds up is a size
				// that does not fit, which is the one outcome this is for.
				size =
					static_cast<int32_t>(std::floor(std::min({byWidth, byHeight, static_cast<float>(size)})));
			}

			if (limits != nullptr) {
				size = std::clamp(size, limits->Min, limits->Max);
			}

			// Never zero: a label drawn at nothing is indistinguishable from one
			// that failed to lay out.
			return std::max(size, 1);
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
				value.TextSize = FittedTextSize(*label, rect.Size(), modifiers.TextLimits);
			}

			store.Set(instance, value);

			// **The clip is intersected here and inherited downwards**, so a
			// node clipped by three ancestors costs three intersections in
			// total rather than three per descendant.
			const Rect inner = element->ClipsDescendants ? clip.Intersection(rect) : clip;
			const Rect area = ContentArea(store, instance, rect, modifiers);

			size_t placed = 1;

			// This level's scratch. Live only until the loops below finish with
			// it: every recursive call uses the next level's, and by the time
			// this one returns nothing is reading it.
			std::vector<Item> &items = ScratchAt(depth);

			// **Marked before the children are measured and released after they
			// are placed.** Measuring each child appends that child's own
			// children, and every one of those runs has to survive until its
			// owner is placed - which is after all of its siblings were measured
			// on top of it. Releasing to the mark at the end drops the whole
			// generation at once, so the arena's high-water mark is the widest
			// path through the tree rather than the tree.
			std::vector<Entity> &arena = ChildArena();
			const ArenaScope scope(arena);

			ChildItems(store, scan, area.Size(), depth, SortsByName(modifiers), items, arena);

			if (modifiers.List != nullptr) {
				placed += RunList(store, *modifiers.List, items, area, inner, depth + 1, total);
			} else if (modifiers.Grid != nullptr) {
				placed += RunGrid(store, *modifiers.Grid, items, area, inner, depth + 1, total);
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

		// The rectangle one collector's roots lay out inside.
		//
		// @return `false` for a collector that has no canvas - a `PluginGui`
		//         today - so nothing under it is laid out rather than being laid
		//         out against a rectangle nobody chose.
		bool CanvasFor(const Store &store, Entity collector, const Screen &screen, Rect &out) {
			const Ids &ids = Classes();

			if (store.IsA(collector, ids.ScreenGui)) {
				const Layer *layer = store.Get<Layer>(collector);
				const float top = layer != nullptr && layer->IgnoreGuiInset ? 0.0f : screen.TopInset;
				out = Rect{Vector2{0.0f, top}, Vector2{screen.Width, screen.Height}};
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
				return true;
			}

			return false;
		}
	}

	size_t Layout(Store &store, const Screen &screen) {
		ENGINE_PROFILE_CAT("gui layout", engine::core::ProfileCategory::ECS);

		// Forces the class table up before the first `IsA` below, which is
		// what makes a store that has never seen this module still lay out.
		Classes();

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

		const Ids &ids = Classes();

		for (const Entity collector : collectors) {
			const Layer *layer = store.Get<Layer>(collector);
			Rect canvas;

			// **Where it sits decides whether it draws at all**, before
			// anything asks how big it is. A `SurfaceGui` or a `BillboardGui`
			// hangs off something in the world, so `Workspace` is a legal home
			// for it; a `ScreenGui` is the viewer's own overlay and is not in
			// the world at all, so it is not.
			const bool spatial =
				store.IsA(collector, ids.SurfaceGui) || store.IsA(collector, ids.BillboardGui);

			const bool drawn = layer != nullptr && layer->Enabled && Contained(store, collector, spatial) &&
							   CanvasFor(store, collector, screen, canvas);

			if (!drawn) {
				continue;
			}

			store.Set(collector, Canvas{canvas});

			Resolved value;
			value.AbsolutePosition = canvas.Min;
			value.AbsoluteSize = canvas.Size();
			value.Clip = canvas;
			value.Rendered = true;
			store.Set(collector, value);

			std::vector<Entity> roots;
			store.EachChild(collector, [&](Entity child) { roots.push_back(child); });

			for (const Entity root : roots) {
				const Element *element = store.Get<Element>(root);
				if (element == nullptr) {
					continue;
				}

				// **The top of the arena's stack discipline.** Every `Place`
				// below releases what it added, but a root's own child run is
				// added *here* - so without this mark it would survive the call
				// and the arena would grow by one run per root per frame, for as
				// long as the process lived. A leak that is invisible in a test
				// and unbounded in a session is exactly the shape worth spelling
				// out at the one place the recursion is entered.
				std::vector<Entity> &arena = ChildArena();
				const ArenaScope scope(arena);

				Scan scan;
				const Vector2 size = Measure(store, root, canvas.Size(), 1, scan, arena);
				const Vector2 anchored = element->Position.Resolve(canvas.Size());
				const Vector2 corner{
					canvas.Min.X + anchored.X - element->AnchorPoint.X * size.X,
					canvas.Min.Y + anchored.Y - element->AnchorPoint.Y * size.Y,
				};

				// Handed on, like every other call: measuring a node is what
				// finds its scan, and there is no path to `Place` that has not
				// measured first - which is what lets `Place` take a `const
				// Scan &` rather than a pointer it has to test.
				placed += Place(store, root, FromCorner(corner, size), canvas, 1, 0.0f, scan);
			}
		}

		return placed;
	}
}
