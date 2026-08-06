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

		Rect FromCorner(const Vector2 &topLeft, const Vector2 &size) {
			return Rect{topLeft, Vector2{topLeft.X + size.X, topLeft.Y + size.Y}};
		}

		// The parent extents a `UDim2` resolves against, per `SizeConstraint`.
		//
		// `RelativeXX` feeding the parent's *width* to both axes is what keeps
		// a square element square as its parent changes shape — which is the
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
		};

		// One child of a container, with what the layout needs to place it.
		struct Item {
			Entity Node;
			Vector2 Size;
			int32_t Order = 0;

			// Only filled in when the container sorts by name.
			//
			// **`InstanceNameOf` reaches the process-wide name registry**, which
			// is a shared lock and a hash lookup — and `SortOrder::Name` is the
			// rarest of the three. Fetching it for every child of every
			// container on every frame paid that cost for a field two sort
			// orders never read.
			core::Name Label;

			// This child's own modifiers, found while it was measured.
			//
			// **Carried rather than looked up again.** Measuring a child needs
			// its scale, aspect and limits; placing it needs its padding and its
			// layout. Those are the same walk of the same child list, and before
			// this field every element in the tree paid for it twice — once from
			// its parent's `ChildItems` and once from its own `Place`.
			Modifiers Mods;
		};

		Modifiers ModifiersOf(const Store &store, Entity instance) {
			Modifiers found;

			// One walk of the child list rather than seven, because
			// `EachChild` chases a handle per sibling through the directory and
			// a container with a padding, a layout and a constraint would
			// otherwise pay for that walk three times.
			store.EachChild(instance, [&](Entity child) {
				// **A modifier is a `UIComponent` and never a `GuiObject`, so
				// one lookup rules out all seven.** `ChildItems` already relies
				// on exactly this — "does it have an `Element`" is what makes
				// something a `GuiObject` there — and without the same test here
				// the common container pays the full seven per child to
				// discover that a frame is not a padding.
				//
				// The common container is a frame full of frames: an inventory
				// grid, a list of rows, a panel of buttons. Measured over a
				// thousand-element tree this test is most of what `Layout`
				// costs, because seven misses against one hit is the ratio in
				// every real interface — see `engine.gui.bench.interface`.
				if (store.Get<Element>(child) != nullptr) {
					return;
				}

				if (found.Inset == nullptr) {
					found.Inset = store.Get<Padding>(child);
				}
				if (found.List == nullptr) {
					found.List = store.Get<ListLayout>(child);
				}
				if (found.Grid == nullptr) {
					found.Grid = store.Get<GridLayout>(child);
				}
				if (found.Aspect == nullptr) {
					found.Aspect = store.Get<AspectRatio>(child);
				}
				if (found.Limits == nullptr) {
					found.Limits = store.Get<SizeLimits>(child);
				}
				if (found.TextLimits == nullptr) {
					found.TextLimits = store.Get<TextSizeLimits>(child);
				}
				if (found.Factor == nullptr) {
					found.Factor = store.Get<Scale>(child);
				}
			});

			return found;
		}

		// The rectangle a container's children are placed inside.
		//
		// The container's own rectangle, less its padding, less any scroll
		// offset. A scrolling frame's canvas is larger than the frame and moves
		// under it, which is the whole of what scrolling is here.
		Rect ContentArea(const Store &store, Entity instance, const Rect &own, const Modifiers &modifiers) {
			Vector2 size = own.Size();
			Vector2 origin = own.Min;

			if (modifiers.Inset != nullptr) {
				const float left = modifiers.Inset->Left.Resolve(size.X);
				const float right = modifiers.Inset->Right.Resolve(size.X);
				const float top = modifiers.Inset->Top.Resolve(size.Y);
				const float bottom = modifiers.Inset->Bottom.Resolve(size.Y);

				origin = Vector2{origin.X + left, origin.Y + top};
				size = Vector2{size.X - left - right, size.Y - top - bottom};
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
		// unchanged when `Scaled` is off is not a shortcut — a scaled size on an
		// unscaled label would silently override what the author typed.
		int32_t FittedTextSize(const Label &label, const Vector2 &box, const TextSizeLimits *limits) {
			int32_t size = label.Size;

			if (label.Scaled) {
				const size_t characters = std::max<size_t>(label.Text.Text().size(), 1);
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
			const Modifiers *known
		);

		// Resolves one node's size against the rectangle it sits in.
		//
		// Split from `Place` because a container running a list or a grid layout
		// has to know how big its children are *before* it can decide where they
		// go — which is the one thing that cannot be done in a single top-down
		// sweep and is why this function exists at all.
		//
		// @param modifiers Filled in with what the walk found, so that `Place`
		//        can be handed it rather than repeating the walk.
		Vector2 Measure(const Store &store, Entity instance, const Vector2 &parent, Modifiers &modifiers) {
			const Element *element = store.Get<Element>(instance);
			if (element == nullptr) {
				return Vector2::Zero;
			}

			modifiers = ModifiersOf(store, instance);
			const Vector2 size = element->Size.Resolve(Basis(element->Constraint, parent));
			return Constrain(size, modifiers, parent);
		}

		// The children a layout arranges, in the order it arranges them.
		//
		// **`UIComponent`s are excluded and that is not an optimisation.** A
		// `UIPadding` under a frame is a child in the tree; if a list layout
		// stacked it, every padded list would have a blank row where the
		// modifier was. The test is "does it have an `Element`", which is what
		// makes something a `GuiObject`.
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
		// an outer level of the recursion is still holding — a bug that would
		// appear only on trees deeper than whatever the pool happened to have
		// reached.
		//
		// `thread_local` because a `Store` binds its owning thread, so two
		// worlds laying out at once are two threads and must not share this.
		std::vector<Item> &ScratchAt(int depth) {
			static thread_local std::vector<std::vector<Item>> pool(
				static_cast<size_t>(MAXIMUM_DEPTH) + 2
			);
			return pool[static_cast<size_t>(std::clamp(depth, 0, MAXIMUM_DEPTH + 1))];
		}

		// @param named Whether the container's sort order reads `Item::Label`.
		//        Only `SortOrder::Name` does, and the lookup is a trip through
		//        the process-wide name registry — so it is asked for rather than
		//        taken.
		// @param items Cleared and filled. Owned by the caller so that the
		//        per-level scratch above can be handed in.
		void ChildItems(
			const Store &store, Entity instance, const Vector2 &area, bool named, std::vector<Item> &items
		) {
			items.clear();

			store.EachChild(instance, [&](Entity child) {
				const Element *element = store.Get<Element>(child);
				if (element == nullptr || !element->Visible) {
					return;
				}

				Item item;
				item.Node = child;
				item.Order = element->LayoutOrder;
				if (named) {
					item.Label = store.InstanceNameOf(child);
				}
				item.Size = Measure(store, child, area, item.Mods);
				items.push_back(item);
			});
		}

		// Whether a container's sort order needs each child's name.
		bool SortsByName(const Modifiers &modifiers) {
			if (modifiers.List != nullptr) {
				return modifiers.List->Order == SortOrder::Name;
			}
			if (modifiers.Grid != nullptr) {
				return modifiers.Grid->Order == SortOrder::Name;
			}
			// A container with neither runs no sort at all — its children are
			// placed by their own `UDim2` — so no name is read.
			return false;
		}

		void Sort(std::vector<Item> &items, SortOrder order) {
			// `std::stable_sort`, so children that compare equal keep their
			// insertion order — which is what `Custom` means and is also the
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

		// Places a container's children in a stack.
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
			const float gap = layout.Padding.Resolve(horizontal ? span.X : span.Y);

			float extent = 0.0f;
			for (size_t index = 0; index < items.size(); index++) {
				extent += horizontal ? items[index].Size.X : items[index].Size.Y;
				if (index + 1 < items.size()) {
					extent += gap;
				}
			}

			// The alignment along the fill axis positions the whole stack; the
			// one across it positions each item on its own, because items in a
			// stack may differ in the cross extent and centring the block would
			// leave the narrow ones off-centre.
			float along = horizontal ? AlignedStart(span.X, extent, static_cast<int>(layout.Horizontal))
									 : AlignedStart(span.Y, extent, static_cast<int>(layout.Vertical));

			size_t placed = 0;
			for (const Item &item : items) {
				const float across =
					horizontal ? AlignedStart(span.Y, item.Size.Y, static_cast<int>(layout.Vertical))
							   : AlignedStart(span.X, item.Size.X, static_cast<int>(layout.Horizontal));

				const Vector2 corner = horizontal ? Vector2{area.Min.X + along, area.Min.Y + across}
												  : Vector2{area.Min.X + across, area.Min.Y + along};

				placed += Place(
					store, item.Node, FromCorner(corner, item.Size), clip, depth, rotation, &item.Mods
				);
				along += (horizontal ? item.Size.X : item.Size.Y) + gap;
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

			int32_t perLine = layout.MaxCells;
			if (perLine <= 0) {
				// As many as fit, at least one. Zero would divide by nothing a
				// line later and is the value Roblox's own default carries.
				perLine =
					step > 0.0f ? static_cast<int32_t>((track + (horizontal ? gap.X : gap.Y)) / step) : 1;
				perLine = std::max(perLine, 1);
			}

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
					store, items[index].Node, FromCorner(corner, cell), clip, depth, rotation,
					&items[index].Mods
				);
			}

			return placed;
		}

		// Writes one node's `Resolved` and descends.
		//
		// `rect` is where this node goes and is already final — a list layout
		// decided it, or the caller resolved the node's own `UDim2`. Splitting
		// the decision out is what lets one function serve both.
		// @param known The node's modifiers when the caller already walked for
		//        them — which every recursive caller has, because measuring a
		//        child is what produced its size. Null at the roots, where
		//        nothing has measured anything yet.
		size_t Place(
			Store &store,
			Entity instance,
			const Rect &rect,
			const Rect &clip,
			int depth,
			float rotation,
			const Modifiers *known
		) {
			if (depth > MAXIMUM_DEPTH) {
				return 0;
			}

			const Element *element = store.Get<Element>(instance);
			if (element == nullptr) {
				return 0;
			}

			// Not marked unrendered here — every `Resolved` was cleared before
			// the walk began, so a subtree the walk does not enter is already
			// saying it is not on screen. That is one pass over a packed column
			// instead of a recursion per hidden node, and it cannot miss a
			// branch the way a hook on every parenting path can.
			if (!element->Visible) {
				return 0;
			}

			Modifiers walked;
			if (known == nullptr) {
				walked = ModifiersOf(store, instance);
				known = &walked;
			}
			const Modifiers &modifiers = *known;

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
			ChildItems(store, instance, area.Size(), SortsByName(modifiers), items);

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
						store, item.Node, FromCorner(corner, item.Size), inner, depth + 1, total, &item.Mods
					);
				}
			}

			return placed;
		}

		// The rectangle one collector's roots lay out inside.
		//
		// @return `false` for a collector that has no canvas — a `PluginGui`
		//         today — so nothing under it is laid out rather than being laid
		//         out against a rectangle nobody chose.
		bool CanvasFor(const Store &store, Entity collector, const Screen &screen, Rect &out) {
			const Ids &ids = Classes();

			if (store.IsA(collector, ids.ScreenGui)) {
				const Layer *layer = store.Get<Layer>(collector);
				const float top = layer != nullptr && layer->IgnoreGuiInset ? 0.0f : screen.TopInset;
				out = Rect{Vector2{0.0f, top}, Vector2{screen.Width, screen.Height}};
				return true;
			}

			if (const Surface *surface = store.Get<Surface>(collector);
				surface != nullptr && store.IsA(collector, ids.SurfaceGui)) {
				// **`PixelsPerStud` resolves to the fixed size for now**, and
				// that is stated rather than silently approximated: the stud
				// extent of the face comes from the adornee's `Bounds`, which
				// is `scene`'s component and an edge this module may not have.
				// Whoever draws a surface gui knows both and is where the
				// multiplication belongs. Filed as `D00022`.
				out = Rect{Vector2::Zero, surface->CanvasSize};
				return true;
			}

			if (const Billboard *billboard = store.Get<Billboard>(collector);
				billboard != nullptr && store.IsA(collector, ids.BillboardGui)) {
				// Offset only. A billboard's scale is against the *screen* it is
				// projected onto, which is a fact about a camera — so the same
				// argument as the surface case, and the same answer.
				out = Rect{Vector2::Zero, Vector2{billboard->Size.X.Offset, billboard->Size.Y.Offset}};
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
		// reach is an orphan — an element a script created and has not
		// parented, one under a `Part`, one beneath a disabled collector or an
		// invisible ancestor — and Roblox draws none of those.
		//
		// A sweep rather than a hook on every parenting path, which is
		// `scene::Visibility`'s argument in as many words: ancestry is not
		// local, and the path that gets missed is the one where an element
		// draws after being detached. One pass over a packed column, against a
		// recursion per hidden subtree.
		//
		// **Only the flag is cleared.** The rectangles stay, so an element
		// scrolled out of view is distinguishable from one never laid out —
		// `Resolved`'s own comment gives the reason.
		store.Each<Resolved>([](Entity, Resolved &resolved) { resolved.Rendered = false; });

		// **Collected before anything is written.** `Place` writes `Resolved`,
		// which can move a row between archetypes the first time a node gets
		// one — and moving a row out from under the query walking it is exactly
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

		for (const Entity collector : collectors) {
			const Layer *layer = store.Get<Layer>(collector);
			Rect canvas;

			const bool drawn =
				layer != nullptr && layer->Enabled && CanvasFor(store, collector, screen, canvas);

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

				Modifiers modifiers;
				const Vector2 size = Measure(store, root, canvas.Size(), modifiers);
				const Vector2 anchored = element->Position.Resolve(canvas.Size());
				const Vector2 corner{
					canvas.Min.X + anchored.X - element->AnchorPoint.X * size.X,
					canvas.Min.Y + anchored.Y - element->AnchorPoint.Y * size.Y,
				};

				// Handed on, like every other call: measuring the root is what
				// found these, so `Place`'s own walk would be the second one.
				// Nothing in this file reaches `Place` without having measured
				// first, which is why the fallback inside it never runs today —
				// it is there so that a future caller that has not measured is
				// correct rather than fast.
				placed += Place(store, root, FromCorner(corner, size), canvas, 1, 0.0f, &modifiers);
			}
		}

		return placed;
	}
}
