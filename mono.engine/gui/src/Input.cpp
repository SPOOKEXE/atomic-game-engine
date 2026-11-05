#include "CaptureContinuity.hpp"
#include "Modal.hpp"
#include "ModalProjection.hpp"
#include "NavigationAccess.hpp"
#include "PointerFrameFacts.hpp"
#include "PointerSelectors.hpp"
#include "TargetedSemantic.hpp"
#include "TypingFrame.hpp"

#include <engine/core/Log.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/InputContinuity.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

namespace engine::gui {

	namespace {
		// A point in an element's own unrotated space.
		//
		// Turns `point` backwards about the rectangle's centre by `degrees`, so a
		// caller can use the axis-aligned rectangle it already has. Returns the
		// point unchanged for the overwhelmingly common unrotated case, which is
		// one comparison rather than two transcendentals.
		//
		// **Takes the two numbers rather than a `DrawCommand`**, because
		// `ElementsAt` asks the same question of a `Resolved` and a second copy of
		// this arithmetic is a second answer to where a rotated button is - the
		// exact split `D00025` was.
		core::Vector2 Unrotated(float degrees, const core::Rect &bounds, const core::Vector2 &point) {
			if (degrees == 0.0f) {
				return point;
			}

			const core::Vector2 pivot{
				(bounds.Min.X + bounds.Max.X) * 0.5f,
				(bounds.Min.Y + bounds.Max.Y) * 0.5f,
			};

			// **Negated, because this undoes the rotation rather than applying
			// it.** Degrees on the property and radians in the arithmetic, for
			// `InterfaceMesh::TurnOf`'s reason - the two must agree about the
			// sign or a rotated button is clickable in its mirror image.
			constexpr float TO_RADIANS = 3.14159265f / 180.0f;
			const float angle = -degrees * TO_RADIANS;
			const float sine = std::sin(angle);
			const float cosine = std::cos(angle);

			const float x = point.X - pivot.X;
			const float y = point.Y - pivot.Y;

			return core::Vector2{
				pivot.X + x * cosine - y * sine,
				pivot.Y + x * sine + y * cosine,
			};
		}
	}

	namespace {
		using core::Vector2;
		using ecs::ClassId;
		using ecs::Entity;
		using ecs::NULL_ENTITY;
		using ecs::Store;

		ClassId ButtonClass() {
			static const ClassId id = [] {
				RegisterGuiClasses();
				return GuiClass("GuiButton");
			}();
			return id;
		}

		// What a pointer landing on an element does.
		//
		// A `GuiButton` is a target whatever its `Active` says, which is Roblox's
		// rule and is why the tests below are an `or` rather than one test on a
		// field the button constructor sets. Setting the field instead would let
		// a script clear it and produce a button that cannot be clicked with
		// nothing in the tree explaining why.
		//
		// **A `TextBox` is the second class that is, and it had to be.** A press
		// is the only gesture that decides where typing goes, so a box the pick
		// walked *past* could never be focused - clicking one landed on whatever
		// was behind it, which is the state this engine shipped in and which
		// reads as a text field that ignores the mouse. `Entry` is the test for
		// the same reason `Element` is the `GuiObject` test in `ElementsAt`: the
		// component is on that class and on no other.
		//
		// **Three answers rather than two, and the third is what `Interactable`
		// is for.** The two properties are not the same question: `Active`
		// decides whether the click *stops* here, and `Interactable` decides
		// whether anything happens when it does. A greyed-out button therefore
		// still swallows the press - which is what stops the panel behind it
		// reacting to a click aimed at the button - and reports nothing.
		enum class Reach : uint8_t {
			// Not an input target. The walk carries on to what is behind.
			Through,

			// An input target that is switched off. The walk stops and nothing
			// is reported.
			Blocked,

			// An input target. The walk stops here.
			Hit,
		};

		Reach Reaches(const Store &store, Entity instance) {
			const Element *element = store.Get<Element>(instance);

			const bool takes = store.IsA(instance, ButtonClass()) || store.Get<Entry>(instance) != nullptr ||
							   store.Get<NodeCanvasLink>(instance) != nullptr ||
							   (element != nullptr && element->Active);
			if (!takes) {
				return Reach::Through;
			}

			return element == nullptr || element->Interactable ? Reach::Hit : Reach::Blocked;
		}
	}

	namespace {
		template <class AllowFn, class CollectorFn> struct LivePointerAccess {
			Store &World;
			const DrawList &Draw;
			AllowFn &Allow;
			CollectorFn &CollectorFnValue;
			size_t CommandCount() const {
				return Draw.Commands.size();
			}
			const DrawCommand &CommandAt(size_t index) const {
				return Draw.Commands[index];
			}
			std::span<const CollectorTransform> Transforms() const {
				return Draw.Transforms;
			}
			uint8_t Reach(Entity node) const {
				return static_cast<uint8_t>(Reaches(World, node));
			}
			bool IsSpatial(Entity node) const {
				return World.Get<SpatialCanvas>(node) != nullptr;
			}
			bool AllowsNode(Entity node, Entity collector) const {
				return Allow(node, collector);
			}
			bool Allows(Entity node, Entity collector) const {
				return Allow(node, collector);
			}
			bool IsEntry(Entity node) const {
				return World.Get<Entry>(node) != nullptr;
			}
			bool IsLink(Entity node) const {
				return World.Get<NodeCanvasLink>(node) != nullptr;
			}
			Entity Collector(Entity node) const {
				return CollectorFnValue(node);
			}
			Entity Detector(Entity node) const {
				Entity found;
				World.EachChild(node, [&](Entity child) {
					if (found != NULL_ENTITY) return;
					const auto *detector = World.Get<DragDetector>(child);
					if (detector && detector->Enabled) found = child;
				});
				return found;
			}
			const Element *ElementOf(Entity node) const {
				return World.Get<Element>(node);
			}
			const Scrolling *ScrollingOf(Entity node) const {
				return World.Get<Scrolling>(node);
			}
			const ScrollState *ScrollStateOf(Entity node) const {
				return World.Get<ScrollState>(node);
			}
			const ScrollMotion *ScrollMotionOf(Entity node) const {
				return World.Get<ScrollMotion>(node);
			}
			const NodeCanvas *NodeCanvasOf(Entity node) const {
				return World.Get<NodeCanvas>(node);
			}
			const Resolved *ResolvedOf(Entity node) const {
				return World.Get<Resolved>(node);
			}
			bool IsDescendant(Entity node, Entity ancestor) const {
				return World.IsDescendantOf(node, ancestor);
			}
			template <class Callback> void EachGraph(Callback &&callback) const {
				World.Each<const NodeCanvas, const Resolved>(callback);
			}
			template <class Callback> void EachScrolling(Callback &&callback) const {
				World.Each<const Scrolling, const ScrollState, const Resolved>(callback);
			}
		};
	}

	namespace {
		// A routing pass may visit thousands of siblings. Cache their shared
		// ancestors only for this pass, retaining the 256-ancestor limit.
		struct ScopeLookup {
			static constexpr size_t LIMIT = 256;
			static constexpr size_t SLOTS = LIMIT * 2;
			struct Entry {
				Entity Node;
				Entity Scope;
				size_t Steps = 0;
			};

			const Store &World;
			std::array<Entry, SLOTS> Cache{};
			size_t Count = 0;

			size_t Slot(Entity entity) const {
				size_t index = std::hash<uint64_t>{}(entity.Id) % SLOTS;
				while (Cache[index].Node != NULL_ENTITY && Cache[index].Node != entity)
					index = (index + 1) % SLOTS;
				return index;
			}

			Entity Find(Entity entity) {
				std::array<uint64_t, LIMIT> path;
				size_t length = 0;
				size_t steps = 0;
				Entity result;
				bool complete = false;
				while (entity != NULL_ENTITY && length < LIMIT) {
					const Entry &held = Cache[Slot(entity)];
					if (held.Node != NULL_ENTITY && held.Steps <= LIMIT - length) {
						result = held.Scope;
						steps = held.Steps;
						complete = true;
						break;
					}
					path[length++] = entity.Id;
					bool enabled = false;
					World.EachChild(entity, [&](Entity child) {
						const ModalScope *scope = World.Get<ModalScope>(child);
						enabled = enabled || (scope != nullptr && scope->Enabled);
					});
					if (enabled) {
						result = entity;
						complete = true;
						break;
					}
					entity = World.ParentOf(entity);
				}
				if (entity == NULL_ENTITY) complete = true;
				if (!complete) return NULL_ENTITY;
				while (length > 0) {
					const Entity node{path[--length]};
					++steps;
					const size_t slot = Slot(node);
					if (Cache[slot].Node == NULL_ENTITY && Count < LIMIT) {
						Cache[slot] = {node, result, steps};
						++Count;
					}
				}
				return result;
			}
		};

		bool PointerAllowsCollector(const Store &store, const Pointer &pointer, Entity collector) {
			return (pointer.Collector == NULL_ENTITY || pointer.Collector == collector) &&
				   (!pointer.ScreenOnly || store.Get<SpatialCanvas>(collector) == nullptr);
		}

		Vector2 LogicalPoint(const DrawList &list, Entity collector, const Vector2 &point) {
			const auto found = std::find_if(
				list.Transforms.begin(),
				list.Transforms.end(),
				[collector](const CollectorTransform &transform) { return transform.Collector == collector; }
			);
			if (found == list.Transforms.end() || !(found->Scale.X > 0.0f) || !(found->Scale.Y > 0.0f)) {
				return point;
			}
			return Vector2{
				(point.X - found->Origin.X) / found->Scale.X,
				(point.Y - found->Origin.Y) / found->Scale.Y,
			};
		}

		bool ContainsCommand(
			const Store &store, const DrawList &list, const DrawCommand &command, const Vector2 &point
		) {
			const Vector2 logical = LogicalPoint(list, command.Collector, point);
			const Vector2 local = Unrotated(command.Rotation, command.Bounds, logical);
			core::Rect bounds = command.Bounds;
			if (store.Get<NodeCanvasLink>(command.Source) != nullptr && bounds.Height() < 8.0f) {
				const float padding = (8.0f - bounds.Height()) * 0.5f;
				bounds.Min.Y -= padding;
				bounds.Max.Y += padding;
			}
			return bounds.Contains(local) && command.Clip.Contains(logical);
		}

		Entity PickWhere(
			const Store &store, const DrawList &list, const Vector2 &point, Entity collector, bool screenOnly
		) {
			ENGINE_PROFILE_CAT("gui pick", engine::core::ProfileCategory::ECS);
			Pointer pointer;
			pointer.Position = point;
			pointer.Collector = collector;
			pointer.ScreenOnly = screenOnly;
			pointer.Inside = true;
			auto allows = [](Entity, Entity) { return true; };
			auto collectorOf = [](Entity) { return NULL_ENTITY; };
			LivePointerAccess access{const_cast<Store &>(store), list, allows, collectorOf};
			return detail::PointerPick(access, pointer);
		}
	}

	bool ContainsDrawCommand(
		const Store &store, const DrawList &list, const DrawCommand &command, const Vector2 &point
	) {
		return ContainsCommand(store, list, command, point);
	}

	Entity Pick(const Store &store, const DrawList &list, const Vector2 &point) {
		return PickWhere(store, list, point, NULL_ENTITY, false);
	}

	Entity PickInCollector(const Store &store, const DrawList &list, Entity collector, const Vector2 &point) {
		return PickWhere(store, list, point, collector, false);
	}

	Entity PickScreen(const Store &store, const DrawList &list, const Vector2 &point) {
		return PickWhere(store, list, point, NULL_ENTITY, true);
	}

	Entity PickVisibleScreen(const DrawList &list, const Vector2 &point) {
		ENGINE_PROFILE_CAT("gui visible screen pick", engine::core::ProfileCategory::ECS);

		// The compiled list is the canonical paint and clip order. The editor
		// must not repeat the hierarchy walk merely because it selects inactive
		// objects that game input deliberately passes through.
		for (size_t index = list.Commands.size(); index > 0; index--) {
			const DrawCommand &command = list.Commands[index - 1];
			if (command.Spatial) {
				continue;
			}

			const core::Vector2 logical = LogicalPoint(list, command.Collector, point);
			const core::Vector2 local = Unrotated(command.Rotation, command.Bounds, logical);
			if (command.Bounds.Contains(local) && command.Clip.Contains(logical)) {
				return command.Source;
			}
		}
		return NULL_ENTITY;
	}

	size_t ElementsAt(const Store &store, Entity root, const Vector2 &point, std::vector<Entity> &found) {
		ENGINE_PROFILE_CAT("gui elements at", engine::core::ProfileCategory::ECS);

		found.clear();
		if (root == NULL_ENTITY) {
			return 0;
		}

		// Iterative, for `EachDescendant`'s reason one module along: how deep a
		// tree goes is the author's to choose, and recursion would put it on the
		// C stack.
		std::vector<Entity> pending;
		store.EachChild(root, [&](Entity child) { pending.push_back(child); });

		while (!pending.empty()) {
			const Entity node = pending.back();
			pending.pop_back();
			if (IsPlayerGuiSource(store, node)) continue;

			// **Descended into before it is tested**, because a container that
			// misses is not a reason to skip what it holds - a `Frame` sized to
			// nothing is a perfectly ordinary way to group elements that are not.
			store.EachChild(node, [&](Entity child) { pending.push_back(child); });

			const Resolved *resolved = store.Get<Resolved>(node);
			if (resolved == nullptr || !resolved->Rendered) {
				continue;
			}

			// **`Element` is the `GuiObject` test.** A `LayerCollector` has no
			// such component - `gui/AGENTS.md` states that the two are not the
			// same class of thing - so this needs no class lookup to leave a
			// nested `ScreenGui` out of the answer.
			if (store.Get<Element>(node) == nullptr) {
				continue;
			}

			const core::Rect bounds{
				resolved->AbsolutePosition,
				Vector2{
					resolved->AbsolutePosition.X + resolved->AbsoluteSize.X,
					resolved->AbsolutePosition.Y + resolved->AbsoluteSize.Y,
				}
			};

			// The clip axis-aligned and the bounds rotated, which is `Pick`'s
			// split and for its reason: a scissor is upright on every backend
			// there is.
			if (!bounds.Contains(Unrotated(resolved->AbsoluteRotation, bounds, point)) ||
				!resolved->Clip.Contains(point)) {
				continue;
			}

			found.push_back(node);
		}

		// Front to back. `Order` descending is the compile's paint order read
		// backwards; `Depth` is the tiebreak for a world nothing has compiled,
		// where every `Order` is zero and the deeper element is still the one in
		// front.
		std::sort(found.begin(), found.end(), [&](Entity left, Entity right) {
			const Resolved *a = store.Get<Resolved>(left);
			const Resolved *b = store.Get<Resolved>(right);
			if (a->Order != b->Order) {
				return a->Order > b->Order;
			}
			if (a->Depth != b->Depth) {
				return a->Depth > b->Depth;
			}

			// **The entity, so the answer does not depend on the walk.** The
			// stack above pops children in reverse sibling order, which is an
			// order nothing else in this module promises - and two elements with
			// one `Order` and one `Depth` would otherwise come back differently
			// depending on how the tree was built.
			return left.Id > right.Id;
		});
		return found.size();
	}

	namespace {
		// How far one notch of the wheel moves a canvas, in pixels.
		//
		// **A constant here rather than a number the host scales its wheel event
		// by**, so a list moves the same distance whether the notch arrived from
		// SDL, from the editor's viewport or from a test. Three lines of ordinary
		// text at the default size, which is what every desktop toolkit settles
		// on and what a person's hand expects.
		constexpr float WHEEL_PIXELS = 60.0f;

		// Whether a frame may be scrolled along one axis at all.
		bool Scrolls(const Scrolling &scrolling, const ScrollState &state, bool vertical) {
			if (!scrolling.Enabled) {
				return false;
			}
			const auto axes = static_cast<uint8_t>(scrolling.Direction);
			const auto wanted =
				static_cast<uint8_t>(vertical ? ScrollingDirection::Y : ScrollingDirection::X);
			if ((axes & wanted) == 0) {
				return false;
			}
			return vertical ? state.CanvasSize.Y > state.WindowSize.Y
							: state.CanvasSize.X > state.WindowSize.X;
		}

		// Moves one frame's canvas and clamps it to what there is to see.
		//
		// **The clamp is here, at the writer.** `gui::ContentArea` clamps what it
		// *reads* so a script assigning past the end still stops at the end, and
		// this clamps what it writes so a wheel cannot accumulate a position the
		// frame will never use. Neither is redundant: one protects the layout
		// from a script and the other protects the property from the router.
		void Move(Scrolling &scrolling, const ScrollState &state, float x, float y) {
			const float rangeX = std::max(state.CanvasSize.X - state.WindowSize.X, 0.0f);
			const float rangeY = std::max(state.CanvasSize.Y - state.WindowSize.Y, 0.0f);
			scrolling.CanvasPosition = core::Vector2{
				std::clamp(scrolling.CanvasPosition.X + x, 0.0f, rangeX),
				std::clamp(scrolling.CanvasPosition.Y + y, 0.0f, rangeY),
			};
		}
	}

	bool Scroll(Store &store, Entity frame, float notches) {
		const ScrollState *state = store.Get<ScrollState>(frame);
		Scrolling *scrolling = store.GetMutable<Scrolling>(frame);
		if (state == nullptr || scrolling == nullptr) {
			return false;
		}

		// **Vertical first, because a frame that scrolls both is a page and a
		// wheel on a page means down.** A horizontal-only frame takes the same
		// turn sideways, which is the only reading that leaves the wheel useful
		// on one.
		if (Scrolls(*scrolling, *state, true)) {
			Move(*scrolling, *state, 0.0f, -notches * WHEEL_PIXELS);
			return true;
		}
		if (Scrolls(*scrolling, *state, false)) {
			Move(*scrolling, *state, -notches * WHEEL_PIXELS, 0.0f);
			return true;
		}
		return false;
	}

	std::span<const GuiEvent>
	Router::UpdateSemantic(Store &store, const DrawList &list, SemanticAction action) {
		Events.clear();
		SyncModals(store, list);
		// The default only covers non-navigating actions. They return before the
		// value reaches SelectNext, while the four navigation cases replace it.
		SelectionMove move = SelectionMove::Up;
		bool navigates = true;
		switch (action) {
		case SemanticAction::Up:
			move = SelectionMove::Up;
			break;
		case SemanticAction::Down:
			move = SelectionMove::Down;
			break;
		case SemanticAction::Left:
			move = SelectionMove::Left;
			break;
		case SemanticAction::Right:
			move = SelectionMove::Right;
			break;
		default:
			navigates = false;
			break;
		}
		const Entity service = GuiServiceOf(store);
		const GuiServiceState *state = store.Get<GuiServiceState>(service);
		const Entity selected = state != nullptr ? state->SelectedObject : NULL_ENTITY;
		Entity collector = CollectorOf(store, selected);
		bool modalCollectorAmbiguous = false;
		if (collector == NULL_ENTITY) {
			Entity onlyModalCollector;
			for (const ModalContext &context : ModalContexts) {
				if (context.Modal == NULL_ENTITY) {
					continue;
				}
				if (onlyModalCollector != NULL_ENTITY) {
					onlyModalCollector = NULL_ENTITY;
					modalCollectorAmbiguous = true;
					break;
				}
				onlyModalCollector = context.Collector;
			}
			collector = onlyModalCollector;
		}
		const Entity modal = ModalFor(collector);
		if (navigates) {
			if (modalCollectorAmbiguous) {
				// A keyboard action has no projected-canvas identity. Seeding from
				// the whole list here could select through either modal, so wait for
				// a routed collector or an existing selection to identify one.
				return Events;
			}
			DrawList scoped;
			const DrawList *visible = &list;
			if (collector != NULL_ENTITY) {
				for (const DrawCommand &command : list.Commands) {
					if (command.Collector != collector) {
						continue;
					}
					if (modal == NULL_ENTITY || command.Source == modal ||
						store.IsDescendantOf(command.Source, modal)) {
						scoped.Commands.push_back(command);
					}
				}
				visible = &scoped;
			}
			(void)SelectNext(store, *visible, move);
			return Events;
		}

		if (action == SemanticAction::Cancel) {
			const Entity focused = FocusedTextBox(store);
			if (Focus(store, NULL_ENTITY)) {
				Events.push_back(
					GuiEvent{
						.Kind = EventKind::FocusReleased,
						.Instance = focused,
						.Position = Vector2::Zero,
						.Local = Vector2::Zero,
						.Entered = false,
						.Collection = NULL_ENTITY,
						.Key = {},
						.Index = UINT32_MAX,
					}
				);
			}
			return Events;
		}

		if (modal != NULL_ENTITY && selected != modal && !store.IsDescendantOf(selected, modal)) {
			return Events;
		}
		const Element *element = store.Get<Element>(selected);
		const bool compiled =
			std::any_of(list.Commands.begin(), list.Commands.end(), [&](const DrawCommand &command) {
				return command.Source == selected;
			});
		if (selected != NULL_ENTITY && store.IsA(selected, ButtonClass()) && element != nullptr &&
			element->Visible && element->Selectable && element->Interactable && compiled) {
			// Keyboard and gamepad activation is not a fabricated pointer press.
			// `Activated` is the semantic event scripts need; claiming a mouse
			// down and up would make device-specific handlers lie.
			Events.push_back(
				GuiEvent{
					.Kind = EventKind::Activated,
					.Instance = selected,
					.Position = Vector2::Zero,
					.Local = Vector2::Zero,
					.Entered = false,
					.Collection = NULL_ENTITY,
					.Key = {},
					.Index = UINT32_MAX,
				}
			);
		}
		return Events;
	}

	std::span<const GuiEvent>
	Router::UpdateSemantic(Store &store, const DrawList &list, Entity target, SemanticAction action) {
		Events.clear();
		SyncModals(store, list);
		if (target == NULL_ENTITY ||
			(action != SemanticAction::Activate && action != SemanticAction::Focus)) {
			return Events;
		}

		const DrawCommand *command = nullptr;
		for (const DrawCommand &candidate : list.Commands) {
			if (candidate.Source != target || !candidate.Bounds.Intersects(candidate.Clip)) {
				continue;
			}
			command = &candidate;
			break;
		}
		const Element *element = store.Get<Element>(target);
		if (command == nullptr || element == nullptr || !element->Visible || !element->Interactable ||
			!AllowsModal(store, target)) {
			return Events;
		}

		if (action == SemanticAction::Focus) {
			if (store.Get<Entry>(target) != nullptr) {
				const Entity previous = FocusedTextBox(store);
				if (Focus(store, target)) {
					if (previous != NULL_ENTITY) {
						Events.push_back(
							{.Kind = EventKind::FocusReleased,
							 .Instance = previous,
							 .Position = Vector2::Zero,
							 .Local = Vector2::Zero,
							 .Collection = NULL_ENTITY,
							 .Key = {},
							 .Index = UINT32_MAX}
						);
					}
					Events.push_back(
						{.Kind = EventKind::Focused,
						 .Instance = target,
						 .Position = Vector2::Zero,
						 .Local = Vector2::Zero,
						 .Collection = NULL_ENTITY,
						 .Key = {},
						 .Index = UINT32_MAX}
					);
				}
			} else if (element->Selectable) {
				(void)Select(store, target);
			}
			return Events;
		}

		if (store.IsA(target, ButtonClass()) && element->Selectable) {
			Events.push_back(
				{.Kind = EventKind::Activated,
				 .Instance = target,
				 .Position = Vector2::Zero,
				 .Local = Vector2::Zero,
				 .Collection = NULL_ENTITY,
				 .Key = {},
				 .Index = UINT32_MAX}
			);
		}
		return Events;
	}

	namespace {
		struct PointerWheelDecision {
			Entity Target;
			Vector2 Origin;
			Vector2 Point;
			bool Graph = false;
		};
		template <class CollectorOf, class Allows>
		PointerWheelDecision FindPointerWheel(
			Store &store,
			const DrawList &list,
			const Pointer &pointer,
			CollectorOf &&collectorOf,
			Allows &&allows
		) {
			LivePointerAccess access{store, list, allows, collectorOf};
			const auto result = detail::PointerWheel(access, pointer);
			return {result.Target, result.Origin, result.Point, result.Graph};
		}
	}

	Entity Router::Wheel(Store &store, const DrawList &list, const Pointer &pointer) {
		const float notches = pointer.Wheel;
		const auto wheel = FindPointerWheel(
			store,
			list,
			pointer,
			[&](Entity node) { return CollectorOf(store, node); },
			[&](Entity node, Entity) { return AllowsModal(store, node); }
		);
		if (NodeCanvas *canvas = store.GetMutable<NodeCanvas>(wheel.Target)) {
			// Multiplicative steps keep the same feel at every scale. The bounds
			// remain authored properties, so a script can deliberately restrict a
			// graph further than this normal interaction permits.
			if (!std::isfinite(notches) || !std::isfinite(canvas->Zoom) ||
				!std::isfinite(canvas->MinimumZoom) || !std::isfinite(canvas->MaximumZoom)) {
				return wheel.Target;
			}
			const float low = std::min(canvas->MinimumZoom, canvas->MaximumZoom);
			const float high = std::max(canvas->MinimumZoom, canvas->MaximumZoom);
			if (!(low > 0.0f) || !(high > 0.0f)) {
				return wheel.Target;
			}
			const float previousZoom = canvas->Zoom;
			const float zoom = std::clamp(previousZoom * std::pow(1.1f, notches), low, high);
			if (!(zoom > 0.0f) || zoom == previousZoom) {
				return wheel.Target;
			}

			const Vector2 relative{wheel.Point.X - wheel.Origin.X, wheel.Point.Y - wheel.Origin.Y};
			// Keep the canvas coordinate below the pointer fixed. Scaling around the
			// canvas origin makes nodes and their labels slide away under the cursor.
			canvas->Pan = Vector2{
				canvas->Pan.X + relative.X / previousZoom - relative.X / zoom,
				canvas->Pan.Y + relative.Y / previousZoom - relative.Y / zoom,
			};
			canvas->Zoom = zoom;
			return wheel.Target;
		}

		return wheel.Target != NULL_ENTITY && Scroll(store, wheel.Target, notches) ? wheel.Target
																				   : NULL_ENTITY;
	}

	namespace {
		struct PointerBarDecision {
			Entity Target;
			float Grab = 0;
			bool Vertical = false;
			size_t CommandIndex = SIZE_MAX;
		};
		template <class Allows>
		PointerBarDecision
		FindPointerBar(const Store &store, const DrawList &list, const Pointer &pointer, Allows &&allows) {
			auto collector = [](Entity) { return NULL_ENTITY; };
			LivePointerAccess access{const_cast<Store &>(store), list, allows, collector};
			const auto result = detail::PointerBar(access, pointer);
			return {result.Target, result.Grab, result.Vertical, result.Index};
		}
	}

	namespace {
		struct PointerDragDecision {
			Entity Detector;
			Entity Target;
			Vector2 Point;
			core::UDim2 Start;
			float Angle = 0;
			size_t CommandIndex = SIZE_MAX;
		};
		template <class Allows>
		PointerDragDecision
		FindPointerDrag(const Store &store, const DrawList &list, const Pointer &pointer, Allows &&allows) {
			auto collector = [](Entity) { return NULL_ENTITY; };
			LivePointerAccess access{const_cast<Store &>(store), list, allows, collector};
			const auto result = detail::PointerDrag(access, pointer);
			return {result.Detector, result.Target, result.Point, result.Start, result.Angle, result.Index};
		}
	}

	bool Router::BeginDrag(Store &store, const DrawList &list, const Pointer &pointer) {
		const auto drag = FindPointerDrag(store, list, pointer, [&](Entity entity, Entity) {
			return AllowsModal(store, entity);
		});
		if (drag.Detector == NULL_ENTITY) return false;
		Detector = drag.Detector;
		Dragged = drag.Target;
		DragFrom = drag.Point;
		DragStart = drag.Start;
		DragAngle = drag.Angle;
		return true;
	}

	void Router::ContinueDrag(Store &store, const Vector2 &point) {
		const DragDetector *detector = store.Get<DragDetector>(Detector);
		const Resolved *resolved = store.Get<Resolved>(Dragged);
		Element *element = store.GetMutable<Element>(Dragged);
		if (detector == nullptr || element == nullptr || resolved == nullptr) {
			Detector = NULL_ENTITY;
			Dragged = NULL_ENTITY;
			return;
		}

		if (detector->Style == DragStyle::Rotate) {
			// The angle swept about the element's own centre, added to whatever
			// it was turned by when the drag began.
			const Vector2 centre{
				resolved->AbsolutePosition.X + resolved->AbsoluteSize.X * 0.5f,
				resolved->AbsolutePosition.Y + resolved->AbsoluteSize.Y * 0.5f,
			};

			constexpr float TO_DEGREES = 180.0f / 3.14159265f;
			const float began = std::atan2(DragFrom.Y - centre.Y, DragFrom.X - centre.X);
			const float now = std::atan2(point.Y - centre.Y, point.X - centre.X);
			element->Rotation = DragAngle + (now - began) * TO_DEGREES;
			return;
		}

		// **A script's drag moves nothing.** `Scriptable` and the two `Custom`
		// responses hand the gesture over whole, which is what those names mean -
		// the events still fire and `GuiEvent::Local` carries how far the pointer
		// has come.
		if (detector->Style == DragStyle::Scriptable || detector->Response == DragResponse::CustomOffset ||
			detector->Response == DragResponse::CustomScale) {
			return;
		}

		Vector2 moved{point.X - DragFrom.X, point.Y - DragFrom.Y};

		// A line drag keeps only the component along the axis, which is the
		// projection. **An axis of zero length is no line at all**, so
		// `TranslateLineOrPlane` falls back to the plane - which is exactly what
		// its name promises and is the one reading under which the member is not
		// a duplicate of `TranslateLine`.
		const float axisLength = detector->Axis.X * detector->Axis.X + detector->Axis.Y * detector->Axis.Y;
		const bool alongLine = axisLength > 0.0f && (detector->Style == DragStyle::TranslateLine ||
													 detector->Style == DragStyle::TranslateLineOrPlane);

		if (alongLine) {
			const float along = (moved.X * detector->Axis.X + moved.Y * detector->Axis.Y) / axisLength;
			moved = Vector2{detector->Axis.X * along, detector->Axis.Y * along};
		}

		moved = Vector2{
			std::clamp(moved.X, detector->MinTranslation.X, detector->MaxTranslation.X),
			std::clamp(moved.Y, detector->MinTranslation.Y, detector->MaxTranslation.Y),
		};

		// **Kept inside the bound by moving the drag rather than the element**,
		// so releasing and grabbing again does not jump: the translation is what
		// is clamped, and the position follows from it.
		if (detector->BoundingUI != NULL_ENTITY) {
			if (const Resolved *bound = store.Get<Resolved>(detector->BoundingUI)) {
				const Vector2 origin{
					resolved->AbsolutePosition.X - (point.X - DragFrom.X) + moved.X,
					resolved->AbsolutePosition.Y - (point.Y - DragFrom.Y) + moved.Y,
				};
				const Vector2 room{
					std::max(bound->AbsoluteSize.X - resolved->AbsoluteSize.X, 0.0f),
					std::max(bound->AbsoluteSize.Y - resolved->AbsoluteSize.Y, 0.0f),
				};
				moved = Vector2{
					moved.X + std::clamp(
								  bound->AbsolutePosition.X - origin.X,
								  std::min(0.0f, bound->AbsolutePosition.X + room.X - origin.X),
								  std::max(0.0f, bound->AbsolutePosition.X + room.X - origin.X)
							  ),
					moved.Y + std::clamp(
								  bound->AbsolutePosition.Y - origin.Y,
								  std::min(0.0f, bound->AbsolutePosition.Y + room.Y - origin.Y),
								  std::max(0.0f, bound->AbsolutePosition.Y + room.Y - origin.Y)
							  ),
				};
			}
		}

		if (detector->Response == DragResponse::Scale) {
			// A scale is a fraction of the *parent*, which is what the element's
			// own `UDim2` resolves against. Without a parent rectangle there is
			// no fraction to write, so the drag does nothing rather than
			// dividing by zero.
			const Resolved *parent = store.Get<Resolved>(store.ParentOf(Dragged));
			if (parent == nullptr || !(parent->AbsoluteSize.X > 0.0f) || !(parent->AbsoluteSize.Y > 0.0f)) {
				return;
			}
			element->Position = core::UDim2{
				DragStart.X.Scale + moved.X / parent->AbsoluteSize.X,
				DragStart.X.Offset,
				DragStart.Y.Scale + moved.Y / parent->AbsoluteSize.Y,
				DragStart.Y.Offset,
			};
			return;
		}

		element->Position = core::UDim2{
			DragStart.X.Scale,
			DragStart.X.Offset + moved.X,
			DragStart.Y.Scale,
			DragStart.Y.Offset + moved.Y,
		};
	}

	namespace {
		// How much of a drag past the end actually moves the canvas.
		//
		// **Resisted rather than refused, and resisted by a constant rather than
		// by a curve.** A diminishing function reads better under a finger and
		// is a worse thing to test: half is a number a case can assert exactly,
		// and the difference is invisible at the twenty or thirty pixels anybody
		// actually pulls.
		constexpr float ELASTIC_RESISTANCE = 0.5f;

		// The furthest a pull may reach, as a fraction of the window it is in.
		// Bounded so a flick cannot drag the content off the frame entirely.
		constexpr float ELASTIC_REACH = 0.35f;

		// Whether this axis may be pulled past its end at all.
		//
		// `WhenScrollable` is the interesting member: it asks whether there is
		// anything to scroll, which is the same question `Scrolls` answers, so a
		// short list in a tall frame does not rubber-band.
		bool Elastic(const Scrolling &scrolling, const ScrollState &state, bool vertical) {
			switch (scrolling.Elastic) {
			case ElasticBehavior::Never:
				return false;
			case ElasticBehavior::Always:
				return true;
			case ElasticBehavior::WhenScrollable:
				break;
			}
			return Scrolls(scrolling, state, vertical);
		}
	}

	namespace {
		struct PointerCanvasDecision {
			Entity Target;
			Vector2 Point;
			size_t CommandIndex = SIZE_MAX;
		};
		template <class Allows>
		PointerCanvasDecision
		FindPointerCanvas(const Store &store, const DrawList &list, const Pointer &pointer, Allows &&allows) {
			auto collector = [](Entity) { return NULL_ENTITY; };
			LivePointerAccess access{const_cast<Store &>(store), list, allows, collector};
			const auto result = detail::PointerCanvas(access, pointer);
			return {result.Target, result.Point, result.Index};
		}
	}

	bool Router::BeginCanvasDrag(Store &store, const DrawList &list, const Pointer &pointer) {
		const auto selected = FindPointerCanvas(store, list, pointer, [&](Entity entity, Entity) {
			return AllowsModal(store, entity);
		});
		if (selected.Target == NULL_ENTITY) return false;
		Canvas = selected.Target;
		ScrollMotion motion;
		if (const ScrollMotion *had = store.Get<ScrollMotion>(Canvas)) motion = *had;
		motion.Pull = motion.Overshoot;
		motion.Released = core::Vector2{};
		motion.ReleasedAt = -1.0;
		motion.LastPoint = selected.Point;
		motion.Held = true;
		store.Set(Canvas, motion);
		return true;
	}

	void Router::DragCanvas(Store &store, const Vector2 &point) {
		if (Canvas == NULL_ENTITY) {
			return;
		}

		Scrolling *scrolling = store.GetMutable<Scrolling>(Canvas);
		const ScrollState *state = store.Get<ScrollState>(Canvas);
		ScrollMotion *motion = store.GetMutable<ScrollMotion>(Canvas);
		if (scrolling == nullptr || state == nullptr || motion == nullptr) {
			return;
		}

		// **The content follows the hand**, so dragging down reveals what is
		// above - the canvas position moves the other way. That is what every
		// touch surface does and the opposite of what a scroll bar does, which
		// is why this is not `DragBar` with a different grab.
		const Vector2 moved{point.X - motion->LastPoint.X, point.Y - motion->LastPoint.Y};
		motion->LastPoint = point;

		const auto axis = [&](bool vertical) {
			const float delta = vertical ? -moved.Y : -moved.X;
			const float range = std::max(
				vertical ? state->CanvasSize.Y - state->WindowSize.Y
						 : state->CanvasSize.X - state->WindowSize.X,
				0.0f
			);
			const float at = vertical ? scrolling->CanvasPosition.Y : scrolling->CanvasPosition.X;
			float &pull = vertical ? motion->Pull.Y : motion->Pull.X;

			// Inside the range this is an ordinary scroll and the rubber band
			// is not involved at all.
			const float wanted = at + delta + pull;
			if (wanted >= 0.0f && wanted <= range && pull == 0.0f) {
				(vertical ? scrolling->CanvasPosition.Y : scrolling->CanvasPosition.X) =
					std::clamp(at + delta, 0.0f, range);
				return;
			}

			if (!Elastic(*scrolling, *state, vertical)) {
				(vertical ? scrolling->CanvasPosition.Y : scrolling->CanvasPosition.X) =
					std::clamp(at + delta, 0.0f, range);
				pull = 0.0f;
				return;
			}

			// Past an end: the position pins to that end and the surplus
			// becomes pull, resisted and bounded.
			const float pinned = std::clamp(wanted, 0.0f, range);
			const float surplus = wanted - pinned;
			const float reach = (vertical ? state->WindowSize.Y : state->WindowSize.X) * ELASTIC_REACH;

			(vertical ? scrolling->CanvasPosition.Y : scrolling->CanvasPosition.X) = pinned;
			pull = std::clamp(surplus * ELASTIC_RESISTANCE, -reach, reach);
		};

		axis(true);
		axis(false);
		motion->Overshoot = motion->Pull;
	}

	void Router::ReleaseCanvas(Store &store) {
		if (Canvas == NULL_ENTITY) {
			return;
		}

		// **`Held` off and nothing else.** The layout stamps when it was let go
		// of, because the layout is the half of this that has a clock - see
		// `AdvanceScrolling`.
		if (ScrollMotion *motion = store.GetMutable<ScrollMotion>(Canvas)) {
			motion->Held = false;
			motion->ReleasedAt = -1.0;
		}
		Canvas = NULL_ENTITY;
	}

	Entity Router::CollectorOf(const Store &store, Entity entity) const {
		for (size_t depth = 0; entity != NULL_ENTITY && depth < 256; depth++) {
			if (store.IsA(entity, GuiClass("LayerCollector"))) {
				return entity;
			}
			entity = store.ParentOf(entity);
		}
		return NULL_ENTITY;
	}

	Entity Router::ModalFor(Entity collector) const {
		for (const ModalContext &context : ModalContexts) {
			if (context.Collector == collector) {
				return context.Modal;
			}
		}
		return NULL_ENTITY;
	}

	bool Router::AllowsModal(const Store &store, Entity entity) const {
		const Entity modal = ModalFor(CollectorOf(store, entity));
		if (modal == NULL_ENTITY && ModalOverflow) {
			return false;
		}
		return modal == NULL_ENTITY || entity == modal || store.IsDescendantOf(entity, modal);
	}

	void Router::CancelCaptureInCollector(Store &store, Entity collector) {
		if (CollectorOf(store, Holding) == collector) {
			Holding = NULL_ENTITY;
		}
		if (CollectorOf(store, Dragging) == collector) {
			Dragging = NULL_ENTITY;
		}
		if (CollectorOf(store, Detector) == collector || CollectorOf(store, Dragged) == collector) {
			Detector = NULL_ENTITY;
			Dragged = NULL_ENTITY;
		}
		if (CollectorOf(store, Canvas) == collector) {
			ReleaseCanvas(store);
		}
	}

	void Router::SyncModals(Store &store, const DrawList &list) {
		ScopeLookup scopes{store};
		const auto inside = [&](Entity entity, Entity scope) {
			return entity != NULL_ENTITY && (entity == scope || store.IsDescendantOf(entity, scope));
		};

		for (ModalContext &context : ModalContexts) {
			context.NextModal = NULL_ENTITY;
			context.Seen = false;
		}
		ModalOverflow = false;

		// Draw order chooses the innermost visible scope in each collector. The
		// contexts are kept separately because a surface dialog has no authority
		// over the HUD or a second surface. A reverse walk finds each top scope
		// once, avoiding a full draw-list scan for every collector.
		for (size_t index = list.Commands.size(); index > 0; index--) {
			const DrawCommand &command = list.Commands[index - 1];
			if (command.Collector == NULL_ENTITY) {
				continue;
			}
			auto found =
				std::find_if(ModalContexts.begin(), ModalContexts.end(), [&](const ModalContext &context) {
					return context.Collector == command.Collector;
				});
			if (found == ModalContexts.end()) {
				const Entity top = scopes.Find(command.Source);
				if (top == NULL_ENTITY) {
					continue;
				}
				if (ModalContexts.size() >= MAX_MODAL_CONTEXTS) {
					// A new context cannot be tracked safely once the bound is full.
					// `AllowsModal` refuses unknown collectors for this frame.
					ModalOverflow = true;
					continue;
				}
				ModalContexts.push_back(
					ModalContext{
						.Collector = command.Collector,
						.Modal = NULL_ENTITY,
						.NextModal = top,
						.FocusBeforeModal = NULL_ENTITY,
						.SelectionBeforeModal = NULL_ENTITY,
						.Seen = true,
					}
				);
				continue;
			}
			found->Seen = true;
			if (found->NextModal == NULL_ENTITY) {
				found->NextModal = scopes.Find(command.Source);
			}
		}

		for (size_t index = 0; index < ModalContexts.size();) {
			ModalContext &context = ModalContexts[index];
			const Entity top = context.NextModal;
			if (top == context.Modal) {
				if (top == NULL_ENTITY) {
					ModalContexts.erase(ModalContexts.begin() + static_cast<std::ptrdiff_t>(index));
					continue;
				}
				index++;
				continue;
			}

			const bool opened = context.Modal == NULL_ENTITY && top != NULL_ENTITY;
			if (context.Modal != NULL_ENTITY && top == NULL_ENTITY) {
				if (context.FocusBeforeModal != NULL_ENTITY && store.Alive(context.FocusBeforeModal) &&
					CollectorOf(store, context.FocusBeforeModal) == context.Collector &&
					store.Get<Entry>(context.FocusBeforeModal) != nullptr) {
					(void)Focus(store, context.FocusBeforeModal);
				}
				if (context.SelectionBeforeModal != NULL_ENTITY &&
					store.Alive(context.SelectionBeforeModal) &&
					CollectorOf(store, context.SelectionBeforeModal) == context.Collector) {
					(void)Select(store, context.SelectionBeforeModal);
				}
				context.FocusBeforeModal = NULL_ENTITY;
				context.SelectionBeforeModal = NULL_ENTITY;
			}

			// A nested scope replaces the accessible subtree. Any capture outside
			// it must end here, or its later release can activate a hidden control.
			if (top != NULL_ENTITY) {
				CancelCaptureInCollector(store, context.Collector);
			}
			context.Modal = top;
			if (top == NULL_ENTITY) {
				ModalContexts.erase(ModalContexts.begin() + static_cast<std::ptrdiff_t>(index));
				continue;
			}

			const Entity focused = FocusedTextBox(store);
			if (CollectorOf(store, focused) == context.Collector && !inside(focused, top)) {
				if (opened) {
					context.FocusBeforeModal = focused;
				}
				(void)Focus(store, NULL_ENTITY);
			}
			const GuiServiceState *state = store.Get<GuiServiceState>(GuiServiceOf(store));
			if (state != nullptr && CollectorOf(store, state->SelectedObject) == context.Collector &&
				!inside(state->SelectedObject, top)) {
				if (opened) {
					context.SelectionBeforeModal = state->SelectedObject;
				}
				(void)Select(store, NULL_ENTITY);
			}
			index++;
		}
	}

	void Router::DragBar(Store &store, const Vector2 &point) {
		const Scrolling *scrolling = store.Get<Scrolling>(Dragging);
		const ScrollState *state = store.Get<ScrollState>(Dragging);
		const Resolved *resolved = store.Get<Resolved>(Dragging);
		if (scrolling == nullptr || state == nullptr || resolved == nullptr) {
			Dragging = NULL_ENTITY;
			return;
		}

		const core::Rect &thumb = DragVertical ? state->VerticalThumb : state->HorizontalThumb;
		if (thumb.Empty()) {
			return;
		}

		// The track is the frame's own extent along the dragged axis, and the
		// travel is what the thumb has left over. A canvas position is then the
		// same fraction of its own range - which is `ThumbAlong` inverted, and it
		// has to be, or the thumb would not end up back under the pointer.
		const float window = DragVertical ? state->WindowSize.Y : state->WindowSize.X;
		const float canvas = DragVertical ? state->CanvasSize.Y : state->CanvasSize.X;
		const float length = DragVertical ? thumb.Height() : thumb.Width();
		const float travel = std::max(window - length, 0.0f);
		const float range = std::max(canvas - window, 0.0f);
		if (!(travel > 0.0f) || !(range > 0.0f)) {
			return;
		}

		const core::Rect bounds{
			resolved->AbsolutePosition, resolved->AbsolutePosition + resolved->AbsoluteSize
		};
		const Vector2 local = Unrotated(resolved->AbsoluteRotation, bounds, point);
		const float origin = DragVertical ? resolved->AbsolutePosition.Y : resolved->AbsolutePosition.X;
		const float at = (DragVertical ? local.Y : local.X) - origin - DragGrab;
		const float wanted = std::clamp(at / travel, 0.0f, 1.0f) * range;

		Scrolling *writable = store.GetMutable<Scrolling>(Dragging);
		if (writable == nullptr) {
			return;
		}

		if (DragVertical) {
			writable->CanvasPosition.Y = wanted;
		} else {
			writable->CanvasPosition.X = wanted;
		}
	}

	Entity Router::CapturedCollector(const Store &store) const {
		const Entity captured = Holding != NULL_ENTITY	  ? Holding
								: Dragging != NULL_ENTITY ? Dragging
								: Canvas != NULL_ENTITY	  ? Canvas
														  : Dragged;
		return CollectorOf(store, captured);
	}

	Router::ModalPreview Router::PreviewModals(const Store &store, const DrawList &list) const {
		// Preview the same modal contexts as Update without changing focus or capture.
		ScopeLookup scopes{store};
		ModalPreview result;
		auto &contexts = result.Contexts;
		size_t &count = result.Count;
		count = ModalContexts.size();
		for (size_t index = 0; index < count; ++index) {
			contexts[index] = ModalContexts[index];
			contexts[index].NextModal = NULL_ENTITY;
		}
		bool &overflow = result.Overflow;
		for (size_t index = list.Commands.size(); index > 0; --index) {
			const DrawCommand &command = list.Commands[index - 1];
			if (command.Collector == NULL_ENTITY) continue;
			size_t slot = 0;
			while (slot < count && contexts[slot].Collector != command.Collector)
				++slot;
			if (slot == count) {
				const Entity top = scopes.Find(command.Source);
				if (top == NULL_ENTITY) continue;
				if (count == contexts.size()) {
					overflow = true;
					continue;
				}
				contexts[count].Collector = command.Collector;
				contexts[count++].NextModal = top;
			} else if (contexts[slot].NextModal == NULL_ENTITY) {
				contexts[slot].NextModal = scopes.Find(command.Source);
			}
		}
		return result;
	}

	namespace {
		std::optional<int32_t>
		FindPointerTextCaret(const DrawList &list, const Pointer &pointer, Entity wanted) {
			struct Access {
				const DrawList &List;
				size_t CommandCount() const {
					return List.Commands.size();
				}
				const DrawCommand &CommandAt(size_t index) const {
					return List.Commands[index];
				}
				std::span<const CollectorTransform> Transforms() const {
					return List.Transforms;
				}
			};
			return detail::PointerCaret(Access{list}, pointer, wanted);
		}
	}

	RouterCaptureWitness::RouterCaptureWitness() = default;
	RouterCaptureWitness::~RouterCaptureWitness() = default;
	RouterCaptureWitness::RouterCaptureWitness(RouterCaptureWitness &&) noexcept = default;
	RouterCaptureWitness &RouterCaptureWitness::operator=(RouterCaptureWitness &&) noexcept = default;
	bool RouterCaptureWitness::Valid() const {
		return State != nullptr;
	}
	size_t RouterCaptureWitness::RetainedBytes() const {
		return State ? sizeof(RouterCaptureWitness) + State->RetainedBytes : 0;
	}

	void Router::CollectCaptureState(const Store &store, detail::CaptureStateStorage &storage) const {
		storage = {};
		if (ModalContexts.size() > storage.Modals.size()) return;
		const auto owner = [&](Entity entity) {
			return detail::CaptureOwner{entity, CollectorOf(store, entity)};
		};
		auto &state = storage.State;
		state.Holding = owner(Holding);
		state.Dragging = owner(Dragging);
		state.Detector = owner(Detector);
		state.Dragged = owner(Dragged);
		state.Canvas = owner(Canvas);
		state.Over = Over;
		state.Last = Last;
		state.DragFrom = DragFrom;
		state.DragStart = DragStart;
		state.DragAngle = DragAngle;
		state.Started = Started;
		state.WasDown = WasDown;
		state.HoldingCollection = HoldingCollection;
		state.HoldingKey = HoldingKey;
		state.HoldingIndex = HoldingIndex;
		state.ModalOverflow = ModalOverflow;
		for (size_t i = 0; i < ModalContexts.size(); ++i) {
			const auto &context = ModalContexts[i];
			storage.Modals[i] = {context.Modal, context.Collector};
			storage.SavedFocus[i] = owner(context.FocusBeforeModal);
			storage.SavedSelection[i] = owner(context.SelectionBeforeModal);
		}
		state.Modals = std::span(storage.Modals.data(), ModalContexts.size());
		state.SavedFocus = std::span(storage.SavedFocus.data(), ModalContexts.size());
		state.SavedSelection = std::span(storage.SavedSelection.data(), ModalContexts.size());
		storage.Valid = true;
	}

	RouterCaptureWitness
	Router::CaptureRouterContinuity(const Store &store, size_t byteBudget, uint64_t routerIncarnation) const {
		RouterCaptureWitness result;
		if (byteBudget < sizeof(result)) return result;
		detail::CaptureStateStorage storage;
		CollectCaptureState(store, storage);
		if (!storage.Valid) return result;
		auto value = detail::CaptureRouterContinuity(
			store, storage.State, byteBudget - sizeof(result), routerIncarnation
		);
		if (!value) return result;
		try {
			result.State = std::make_unique<detail::CaptureContinuity>(std::move(*value));
		} catch (const std::bad_alloc &) {}
		return result;
	}

	bool Router::MatchesRouterContinuity(
		const Store &store, const RouterCaptureWitness &witness, uint64_t routerIncarnation
	) const {
		if (!witness.Valid()) return false;
		detail::CaptureStateStorage storage;
		CollectCaptureState(store, storage);
		return storage.Valid &&
			   detail::MatchesRouterContinuity(store, *witness.State, storage.State, routerIncarnation);
	}

	PointerFrame::PointerFrame() = default;
	PointerFrame::~PointerFrame() = default;
	PointerFrame::PointerFrame(PointerFrame &&) noexcept = default;
	PointerFrame &PointerFrame::operator=(PointerFrame &&) noexcept = default;
	bool PointerFrame::Valid() const {
		return State && State->Valid;
	}
	size_t PointerFrame::RetainedBytes() const {
		if (!Valid()) return 0;
		size_t total = sizeof(detail::PointerFrameFacts);
		const auto charge = [&](const auto &container) {
			using Value = typename std::decay_t<decltype(container)>::value_type;
			constexpr size_t overhead = 64;
			const size_t count = container.capacity();
			if (!count) return;
			if (count > (std::numeric_limits<size_t>::max() - overhead) / sizeof(Value)) {
				total = std::numeric_limits<size_t>::max();
				return;
			}
			const size_t bytes = count * sizeof(Value) + overhead;
			if (total > std::numeric_limits<size_t>::max() - bytes)
				total = std::numeric_limits<size_t>::max();
			else
				total += bytes;
		};
		charge(State->Commands);
		charge(State->Nodes);
		charge(State->GraphWheelNodes);
		charge(State->ScrollWheelNodes);
		charge(State->CollectorTransforms);
		charge(State->Modals);
		charge(State->SpatialCanvases);
		for (const auto &node : State->Nodes)
			charge(node.EntryText);
		for (const auto &command : State->Commands) {
			charge(command.Key);
			charge(command.Shaping.Glyphs);
			charge(command.Shaping.Runs);
			charge(command.Shaping.Lines);
			charge(command.Shaping.CaretStops);
			charge(command.Shaping.GraphemeBoundaries);
			charge(command.Shaping.LineBreakBoundaries);
		}
		const auto chargeWitness = [&](size_t bytes) {
			if (total > std::numeric_limits<size_t>::max() - bytes)
				total = std::numeric_limits<size_t>::max();
			else
				total += bytes;
		};
		if (State->InitialInput) chargeWitness(State->InitialInput->RetainedBytes());
		if (State->InitialRouter) chargeWitness(State->InitialRouter->RetainedBytes());
		return total;
	}
	const InputContinuity *PointerFrame::InitialInputContinuity() const {
		return Valid() && State->InitialInput ? &*State->InitialInput : nullptr;
	}
	const RouterCaptureWitness *PointerFrame::InitialRouterContinuity() const {
		return Valid() && State->InitialRouter ? &*State->InitialRouter : nullptr;
	}
	uint64_t PointerFrame::StoreIdentity() const {
		return Valid() ? State->StoreIdentity : 0;
	}
	core::Vector2 PointerFrame::CanvasSize() const {
		return Valid() ? State->CanvasSize : core::Vector2{};
	}
	bool PointerFrame::ExpectsScreenLayer() const {
		return Valid() && State->ExpectsScreen;
	}
	bool PointerFrame::ExpectsSpatialLayer() const {
		return Valid() && State->ExpectsSpatial;
	}
	bool PointerFrame::HasScreenCommands() const {
		return Valid() && State->HasScreenCommands;
	}
	bool PointerFrame::HasSpatialCommands() const {
		return Valid() && State->HasSpatialCommands;
	}
	bool PointerFrame::LayerClassificationValid() const {
		return Valid() && State->LayerClassificationValid;
	}
	size_t PointerFrame::SpatialCanvasCount() const {
		return Valid() ? State->SpatialCanvases.size() : 0;
	}
	bool PointerFrame::SpatialCanvasAt(size_t index, Entity &collector, SpatialCanvas &canvas) const {
		if (!Valid() || index >= State->SpatialCanvases.size()) return false;
		collector = State->SpatialCanvases[index].Collector;
		canvas = State->SpatialCanvases[index].Canvas;
		return true;
	}
	PointerFrameStatus PointerFrame::PointerStatus(const Pointer &pointer) const {
		if (!Valid()) return PointerFrameStatus::Invalid;
		if (!pointer.Inside || pointer.Cancelled) return PointerFrameStatus::Pass;
		size_t hits = 0, keyBytes = 0;
		bool sawInput = false, takesInput = false;
		for (size_t index = State->CommandCount(); index > 0; --index) {
			const auto &command = State->CommandAt(index - 1);
			if (!detail::PointerCollectorAllowed(*State, pointer, command.Collector) ||
				!detail::PointerContains(*State, command, pointer.Position))
				continue;
			if (hits == MAX_POINTER_HITS || command.Key.size() > MAX_POINTER_KEY_BYTES - keyBytes)
				return PointerFrameStatus::Invalid;
			++hits;
			keyBytes += command.Key.size();
			if (!sawInput && State->Reach(command.Source) != 0) {
				sawInput = true;
				takesInput = State->Allows(command.Source, State->Collector(command.Source));
			}
		}
		if (takesInput) return PointerFrameStatus::Consume;
		if (pointer.Down && (detail::PointerBar(*State, pointer).Target != NULL_ENTITY ||
							 detail::PointerDrag(*State, pointer).Detector != NULL_ENTITY ||
							 detail::PointerCanvas(*State, pointer).Target != NULL_ENTITY))
			return PointerFrameStatus::Consume;
		if (pointer.Wheel != 0 && detail::PointerWheel(*State, pointer).Target != NULL_ENTITY)
			return PointerFrameStatus::Consume;
		return PointerFrameStatus::Pass;
	}

	ecs::Entity PointerFrame::VisibleAt(const Pointer &pointer) const {
		return Valid() ? detail::PointerVisible(*State, pointer).Source : NULL_ENTITY;
	}
	size_t PointerFrame::VisiblePaintOrder(const Pointer &pointer) const {
		return Valid() ? detail::PointerVisible(*State, pointer).Order : 0;
	}
	size_t PointerFrame::PaintOrder(const Pointer &pointer) const {
		if (!Valid() || !pointer.Inside) return 0;
		for (size_t index = State->CommandCount(); index > 0; --index) {
			const auto &command = State->CommandAt(index - 1);
			if (detail::PointerCollectorAllowed(*State, pointer, command.Collector) &&
				detail::PointerContains(*State, command, pointer.Position))
				return index;
		}
		return 0;
	}
	PointerFrame Router::CapturePointerFrame(Store &store, const DrawList &list) const {
		return CapturePointerFrameWithContinuity(store, list, 0);
	}
	PointerFrame Router::CapturePointerFrameWithContinuity(
		Store &store, const DrawList &list, uint64_t routerIncarnation
	) const {
		PointerFrame result;
		const auto preview = PreviewModals(store, list);
		std::array<detail::PointerFrameModal, MAX_MODAL_CONTEXTS> modals{};
		size_t modalCount = 0;
		for (size_t i = 0; i < preview.Count; ++i) {
			if (preview.Contexts[i].NextModal != NULL_ENTITY)
				modals[modalCount++] = {preview.Contexts[i].Collector, preview.Contexts[i].NextModal};
		}
		std::array<Entity, 10 + 2 * MAX_MODAL_CONTEXTS> roots{};
		size_t rootCount = 0;
		if (routerIncarnation != 0) {
			if (const auto *state = store.Get<GuiServiceState>(GuiServiceOf(store))) {
				roots[rootCount++] = state->FocusedTextBox;
				roots[rootCount++] = FocusedTextBox(store);
				roots[rootCount++] = state->SelectedObject;
			}
			for (Entity e : {Holding, HoldingCollection, Dragging, Detector, Dragged, Canvas, Over})
				roots[rootCount++] = e;
			for (size_t i = 0; i < preview.Count; ++i) {
				roots[rootCount++] = preview.Contexts[i].FocusBeforeModal;
				roots[rootCount++] = preview.Contexts[i].SelectionBeforeModal;
			}
		}
		try {
			auto facts = std::make_unique<detail::PointerFrameFacts>();
			if (detail::CapturePointerFrameFacts(
					store,
					list,
					std::span(modals.data(), modalCount),
					preview.Overflow,
					*facts,
					std::span(roots.data(), rootCount)
				)) {
				constexpr size_t CONTINUITY_BUDGET = 1024 * 1024;
				if (routerIncarnation != 0) {
					auto initialInput = CaptureInputContinuity(store, CONTINUITY_BUDGET);
					if (initialInput && initialInput->RetainedBytes() < CONTINUITY_BUDGET) {
						auto initialRouter = CaptureRouterContinuity(
							store, CONTINUITY_BUDGET - initialInput->RetainedBytes(), routerIncarnation
						);
						if (initialRouter.Valid()) {
							facts->InitialInput.emplace(std::move(*initialInput));
							facts->InitialRouter.emplace(std::move(initialRouter));
						}
					}
				}
				result.State = std::move(facts);
			}
		} catch (const std::bad_alloc &) {}
		return result;
	}
	bool Router::MatchesPointerFrame(
		Store &store, const DrawList &list, const PointerFrame &frame, const Pointer &pointer
	) const {
		if (!frame.Valid()) return false;
		try {
			return MatchesPointerScene(store, list, detail::SamplePointerFrameFacts(*frame.State, pointer));
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	bool Router::MatchesVisibleFrame(
		Store &store, const DrawList &list, const PointerFrame &frame, const Pointer &pointer
	) const {
		if (!frame.Valid() || frame.StoreIdentity() != store.Identity()) return false;
		auto allows = [](Entity, Entity) { return true; };
		auto collector = [](Entity) { return NULL_ENTITY; };
		LivePointerAccess live{store, list, allows, collector};
		const auto original = detail::PointerVisible(*frame.State, pointer);
		const auto current = detail::PointerVisible(live, pointer);
		return original == current && (original.Source == NULL_ENTITY || store.Alive(original.Source));
	}

	PointerDecision Router::DecidePointer(Store &store, const DrawList &list, const Pointer &pointer) const {
		PointerDecision result;
		result.Sample = pointer;
		result.Store = store.Identity();
		const auto preview = PreviewModals(store, list);
		result.ModalOverflow = preview.Overflow;
		for (size_t i = 0; i < preview.Count; ++i) {
			const auto &scope = preview.Contexts[i];
			if (scope.NextModal == NULL_ENTITY) continue;
			result.Modals.push_back(
				{scope.Collector,
				 scope.Modal,
				 scope.NextModal,
				 scope.FocusBeforeModal,
				 scope.SelectionBeforeModal}
			);
		}
		const auto modalFor = [&](Entity collector) {
			for (size_t i = 0; i < preview.Count; ++i)
				if (preview.Contexts[i].Collector == collector) return preview.Contexts[i].NextModal;
			return NULL_ENTITY;
		};
		auto allows = [&](Entity entity, Entity) {
			const Entity modal = modalFor(CollectorOf(store, entity));
			if (modal == NULL_ENTITY && preview.Overflow) return false;
			return modal == NULL_ENTITY || entity == modal || store.IsDescendantOf(entity, modal);
		};
		auto collector = [&](Entity entity) { return CollectorOf(store, entity); };
		LivePointerAccess access{store, list, allows, collector};
		result.Hovered = detail::PointerPick(access, pointer);
		if (pointer.Inside && pointer.Down) {
			const auto bar = detail::PointerBar(access, pointer);
			const auto drag = detail::PointerDrag(access, pointer);
			const auto canvas = detail::PointerCanvas(access, pointer);
			result.VisualBar = bar.Target;
			result.VisualBarGrab = bar.Grab;
			result.VisualBarVertical = bar.Vertical;
			result.VisualBarState = detail::PointerScrollState(access, bar.Target);
			result.VisualDrag = drag.Target;
			result.VisualDetector = drag.Detector;
			result.VisualDragPoint = drag.Point;
			result.VisualDragStart = drag.Start;
			result.VisualDragAngle = drag.Angle;
			result.VisualCanvas = canvas.Target;
			result.VisualCanvasState = detail::PointerScrollState(access, canvas.Target);
			if (const auto *motion = store.Get<ScrollMotion>(canvas.Target))
				result.VisualCanvasOvershoot = motion->Overshoot;
		}
		if (pointer.Inside && pointer.Wheel != 0) {
			const auto wheel = detail::PointerWheel(access, pointer);
			result.VisualWheel = wheel.Target;
			result.VisualWheelGraph = wheel.Graph;
			result.VisualWheelOrigin = wheel.Origin;
			result.VisualWheelPoint = wheel.Point;
			if (const auto *graph = store.Get<NodeCanvas>(wheel.Target)) {
				result.VisualWheelValues = {
					graph->Pan.X, graph->Pan.Y, graph->Zoom, graph->MinimumZoom, graph->MaximumZoom
				};
			} else if (const auto *scrolling = store.Get<Scrolling>(wheel.Target)) {
				if (const auto *state = store.Get<ScrollState>(wheel.Target)) {
					result.VisualWheelValues = {
						scrolling->CanvasPosition.X,
						scrolling->CanvasPosition.Y,
						state->CanvasSize.X,
						state->CanvasSize.Y,
						state->WindowSize.X,
						state->WindowSize.Y
					};
					result.VisualWheelFlags = static_cast<uint8_t>(scrolling->Direction);
				}
			}
		}
		if (const auto *entry = store.Get<Entry>(result.Hovered)) {
			if (const auto caret = detail::PointerCaret(access, pointer, result.Hovered))
				result.VisualCaret = *caret;
			if (pointer.Inside && pointer.Down) {
				result.VisualEntryClearOnFocus = entry->ClearTextOnFocus;
				if (const auto *label = store.Get<Label>(result.Hovered)) {
					if (label->Text.size() > detail::MAX_POINTER_ENTRY_TEXT_BYTES) {
						result.Valid = false;
						return result;
					}
					result.VisualEntryHasLabel = true;
					result.VisualEntryText = label->Text;
				}
			}
		}
		size_t keyBytes = 0;
		for (size_t index = list.Commands.size(); index > 0; --index) {
			const auto &command = list.Commands[index - 1];
			if (!detail::PointerCollectorAllowed(access, pointer, command.Collector) ||
				!detail::PointerContains(access, command, pointer.Position))
				continue;
			if (result.Hits.size() == MAX_POINTER_HITS ||
				command.Key.size() > MAX_POINTER_KEY_BYTES - keyBytes) {
				result.Valid = false;
				break;
			}
			keyBytes += command.Key.size();
			result.Hits.push_back(
				{command.Source,
				 command.Collector,
				 command.Collection,
				 command.Key,
				 command.Index,
				 command.Bounds,
				 command.Clip,
				 detail::PointerLogical(access, command.Collector, pointer.Position),
				 command.Rotation,
				 static_cast<uint8_t>(Reaches(store, command.Source))}
			);
		}
		return result;
	}

	bool
	Router::MatchesPointerScene(Store &store, const DrawList &list, const PointerDecision &decision) const {
		if (!decision.Valid || store.Identity() != decision.Store) return false;
		const auto current = DecidePointer(store, list, decision.Sample);
		if (!current.Valid || current.Hovered != decision.Hovered || current.Hits != decision.Hits ||
			current.VisualBar != decision.VisualBar || current.VisualBarGrab != decision.VisualBarGrab ||
			current.VisualBarVertical != decision.VisualBarVertical ||
			current.VisualBarState != decision.VisualBarState ||
			current.VisualCanvasState != decision.VisualCanvasState ||
			current.VisualDrag != decision.VisualDrag || current.VisualDetector != decision.VisualDetector ||
			current.VisualDragPoint != decision.VisualDragPoint ||
			current.VisualDragStart != decision.VisualDragStart ||
			current.VisualDragAngle != decision.VisualDragAngle ||
			current.VisualCanvas != decision.VisualCanvas ||
			current.VisualCanvasOvershoot != decision.VisualCanvasOvershoot ||
			current.VisualCaret != decision.VisualCaret ||
			current.VisualEntryClearOnFocus != decision.VisualEntryClearOnFocus ||
			current.VisualEntryHasLabel != decision.VisualEntryHasLabel ||
			current.VisualEntryText != decision.VisualEntryText ||
			current.VisualWheel != decision.VisualWheel ||
			current.VisualWheelGraph != decision.VisualWheelGraph ||
			current.VisualWheelOrigin != decision.VisualWheelOrigin ||
			current.VisualWheelPoint != decision.VisualWheelPoint ||
			current.VisualWheelValues != decision.VisualWheelValues ||
			current.VisualWheelFlags != decision.VisualWheelFlags ||
			current.ModalOverflow != decision.ModalOverflow ||
			current.Modals.size() != decision.Modals.size())
			return false;
		for (size_t index = 0; index < current.Modals.size(); ++index) {
			if (current.Modals[index].Collector != decision.Modals[index].Collector ||
				current.Modals[index].Next != decision.Modals[index].Next)
				return false;
		}
		return true;
	}

	bool Router::WantsPointer(const Store &store, const DrawList &list, const Pointer &pointer) const {
		const ModalPreview preview = PreviewModals(store, list);
		const auto &contexts = preview.Contexts;
		const size_t count = preview.Count;
		const bool overflow = preview.Overflow;
		const auto modalFor = [&](Entity collector) {
			for (size_t index = 0; index < count; ++index) {
				if (contexts[index].Collector == collector) return contexts[index].NextModal;
			}
			return NULL_ENTITY;
		};
		const auto allows = [&](Entity entity, Entity collector) {
			const Entity modal = modalFor(collector);
			if (modal == NULL_ENTITY && overflow) return false;
			return modal == NULL_ENTITY || entity == modal || store.IsDescendantOf(entity, modal);
		};

		const Entity captured = Holding != NULL_ENTITY	  ? Holding
								: Dragging != NULL_ENTITY ? Dragging
								: Canvas != NULL_ENTITY	  ? Canvas
														  : Dragged;
		if (captured != NULL_ENTITY) {
			const Entity collector = CollectorOf(store, captured);
			const Entity next = modalFor(collector);
			// A newly opened or changed scope cancels the old capture in Update.
			if (next == NULL_ENTITY || next == ModalFor(collector)) {
				return PointerAllowsCollector(store, pointer, collector);
			}
		}
		if (!pointer.Inside || pointer.Cancelled) return false;

		for (size_t index = list.Commands.size(); index > 0; --index) {
			const DrawCommand &command = list.Commands[index - 1];
			if (!PointerAllowsCollector(store, pointer, command.Collector) ||
				!ContainsCommand(store, list, command, pointer.Position)) {
				continue;
			}
			if (Reaches(store, command.Source) == Reach::Through) continue;
			if (allows(command.Source, command.Collector)) return true;
			break;
		}

		if (pointer.Down && !WasDown) {
			for (size_t index = list.Commands.size(); index > 0; --index) {
				const DrawCommand &command = list.Commands[index - 1];
				if (!PointerAllowsCollector(store, pointer, command.Collector) ||
					!allows(command.Source, command.Collector)) {
					continue;
				}
				const Vector2 logical = LogicalPoint(list, command.Collector, pointer.Position);
				const Vector2 point = Unrotated(command.Rotation, command.Bounds, logical);
				const Scrolling *scrolling = store.Get<Scrolling>(command.Source);
				const ScrollState *state = store.Get<ScrollState>(command.Source);
				if (scrolling != nullptr && state != nullptr && scrolling->Enabled &&
					command.Clip.Contains(logical)) {
					if (store.Get<Resolved>(command.Source) != nullptr &&
						(state->VerticalThumb.Contains(point) || state->HorizontalThumb.Contains(point))) {
						return true;
					}
					if (command.Bounds.Contains(point) &&
						(Scrolls(*scrolling, *state, true) || Scrolls(*scrolling, *state, false) ||
						 Elastic(*scrolling, *state, true) || Elastic(*scrolling, *state, false))) {
						return true;
					}
				}
				if (store.Get<Element>(command.Source) == nullptr || !command.Bounds.Contains(point) ||
					!command.Clip.Contains(logical)) {
					continue;
				}
				bool detector = false;
				store.EachChild(command.Source, [&](Entity child) {
					const DragDetector *drag = store.Get<DragDetector>(child);
					detector = detector || (drag != nullptr && drag->Enabled);
				});
				if (detector) return true;
			}
		}

		if (pointer.Wheel != 0.0f) {
			std::vector<Entity> pending;
			if (pointer.Collector != NULL_ENTITY) {
				pending.push_back(pointer.Collector);
			} else {
				store.EachRoot([&](Entity root) { pending.push_back(root); });
			}
			while (!pending.empty()) {
				const Entity entity = pending.back();
				pending.pop_back();
				store.EachChild(entity, [&](Entity child) { pending.push_back(child); });
				const Resolved *resolved = store.Get<Resolved>(entity);
				if (resolved == nullptr || !resolved->Rendered) continue;
				const Entity collector = CollectorOf(store, entity);
				if (!PointerAllowsCollector(store, pointer, collector) || !allows(entity, collector))
					continue;
				const NodeCanvas *canvas = store.Get<NodeCanvas>(entity);
				const Scrolling *scrolling = store.Get<Scrolling>(entity);
				const ScrollState *state = store.Get<ScrollState>(entity);
				if (canvas == nullptr &&
					(scrolling == nullptr || state == nullptr ||
					 (!Scrolls(*scrolling, *state, true) && !Scrolls(*scrolling, *state, false)))) {
					continue;
				}
				const Vector2 point = LogicalPoint(list, collector, pointer.Position);
				const core::Rect bounds{
					resolved->AbsolutePosition, resolved->AbsolutePosition + resolved->AbsoluteSize
				};
				if (bounds.Contains(Unrotated(resolved->AbsoluteRotation, bounds, point)) &&
					resolved->Clip.Contains(point))
					return true;
			}
		}
		return false;
	}

	std::span<const GuiEvent> Router::Update(Store &store, const DrawList &list, const Pointer &pointer) {
		ENGINE_PROFILE_CAT("gui route", engine::core::ProfileCategory::ECS);

		Events.clear();
		SyncModals(store, list);

		const Entity picked =
			pointer.Inside ? PickWhere(store, list, pointer.Position, pointer.Collector, pointer.ScreenOnly)
						   : NULL_ENTITY;
		const Entity found = picked != NULL_ENTITY && !AllowsModal(store, picked) ? NULL_ENTITY : picked;

		const auto local = [&](Entity instance) -> Vector2 {
			const Resolved *resolved = store.Get<Resolved>(instance);
			if (resolved == nullptr) {
				return Vector2::Zero;
			}
			Vector2 logical = pointer.Position;
			for (size_t index = list.Commands.size(); index > 0; index--) {
				const DrawCommand &command = list.Commands[index - 1];
				if (command.Source == instance) {
					logical = LogicalPoint(list, command.Collector, pointer.Position);
					break;
				}
			}
			return Vector2{
				logical.X - resolved->AbsolutePosition.X,
				logical.Y - resolved->AbsolutePosition.Y,
			};
		};
		const auto dragPoint = [&]() {
			return LogicalPoint(list, CollectorOf(store, Dragged), pointer.Position);
		};
		const auto commandAt = [&](Entity instance) -> const DrawCommand * {
			for (size_t index = list.Commands.size(); index > 0; --index) {
				const DrawCommand &command = list.Commands[index - 1];
				if (command.Source == instance && PointerAllowsCollector(store, pointer, command.Collector) &&
					ContainsCommand(store, list, command, pointer.Position)) {
					return &command;
				}
			}
			return nullptr;
		};

		const auto emit = [&](EventKind kind, Entity instance) {
			if (instance != NULL_ENTITY && store.Alive(instance)) {
				GuiEvent event{
					kind, instance, pointer.Position, local(instance), false, NULL_ENTITY, {}, UINT32_MAX
				};
				if (instance == Holding && HoldingCollection != NULL_ENTITY) {
					event.Collection = HoldingCollection;
					event.Key = HoldingKey;
					event.Index = HoldingIndex;
				} else if (const DrawCommand *command = commandAt(instance)) {
					event.Collection = command->Collection;
					event.Key = command->Key;
					event.Index = command->Index;
				}
				Events.push_back(std::move(event));
			}
		};

		const auto lostOwner = [&](Entity owner) {
			return owner != NULL_ENTITY && (!store.Alive(owner) || CollectorOf(store, owner) == NULL_ENTITY);
		};
		const bool lostCanvas =
			lostOwner(Canvas) ||
			(Canvas != NULL_ENTITY && (!store.Get<Scrolling>(Canvas) || !store.Get<ScrollState>(Canvas) ||
									   !store.Get<ScrollMotion>(Canvas)));
		const bool lostBar =
			lostOwner(Dragging) || (Dragging != NULL_ENTITY &&
									(!store.Get<Scrolling>(Dragging) || !store.Get<ScrollState>(Dragging) ||
									 !store.Get<Resolved>(Dragging)));
		const bool lostDrag = Detector != NULL_ENTITY &&
							  (lostOwner(Detector) || lostOwner(Dragged) || Dragged == NULL_ENTITY ||
							   !store.Get<DragDetector>(Detector) || !store.Get<Element>(Dragged) ||
							   !store.Get<Resolved>(Dragged) || !store.IsDescendantOf(Detector, Dragged));
		const bool lostHolding = Holding != NULL_ENTITY && !store.Alive(Holding);
		if (pointer.Cancelled || lostCanvas || lostBar || lostDrag || lostHolding) {
			// A lost capture or withdrawn contact cannot activate a target. Keep
			// the physical button level so a held press cannot restart until up.
			Dragging = NULL_ENTITY;
			ReleaseCanvas(store);
			if (Detector != NULL_ENTITY && store.Alive(Detector)) {
				const Vector2 point = dragPoint();
				Events.push_back(
					GuiEvent{
						EventKind::DragEnded,
						Detector,
						pointer.Position,
						Vector2{point.X - DragFrom.X, point.Y - DragFrom.Y},
						false,
						NULL_ENTITY,
						{},
						UINT32_MAX,
					}
				);
			}
			Detector = NULL_ENTITY;
			Dragged = NULL_ENTITY;
			if (store.Alive(Holding)) emit(EventKind::InputEnded, Holding);
			Holding = NULL_ENTITY;
			if (!pointer.Inside && Over != NULL_ENTITY) {
				emit(EventKind::MouseLeave, Over);
				Over = NULL_ENTITY;
			}
			WasDown = pointer.Down;
			Last = pointer.Position;
			Started = true;
			return Events;
		}

		// **Leave before enter**, so a handler that moves something on leave
		// runs before the one that reacts to the arrival. The other order makes
		// a swap between two adjacent buttons produce an enter against state
		// the leave is about to undo.
		if (found != Over) {
			emit(EventKind::MouseLeave, Over);
			emit(EventKind::MouseEnter, found);
			Over = found;
		} else if (Started && (pointer.Position.X != Last.X || pointer.Position.Y != Last.Y)) {
			// `Started` guards the first call: without it a router that has
			// never seen a pointer reports a move from the origin, which fires
			// a `MouseMoved` at whatever happens to be under (0, 0).
			emit(EventKind::MouseMoved, found);
		}

		// **A held bar is followed before anything else and instead of
		// everything else.** A drag that left the thumb still moves the canvas,
		// which is what every scroll bar anywhere does, and the element the
		// pointer happens to be over during it must not light up.
		if (pointer.Down && Dragging != NULL_ENTITY) {
			DragBar(store, LogicalPoint(list, CollectorOf(store, Dragging), pointer.Position));
			Last = pointer.Position;
			WasDown = true;
			Started = true;
			return Events;
		}

		// **A held canvas, on the same footing as a held bar**, and before the
		// detector for the reason the bar is before both: a gesture already in
		// flight owns the pointer until it is let go of.
		if (pointer.Down && Canvas != NULL_ENTITY) {
			DragCanvas(store, LogicalPoint(list, CollectorOf(store, Canvas), pointer.Position));
			Last = pointer.Position;
			WasDown = true;
			Started = true;
			return Events;
		}

		// **A held drag detector, on the same footing as a held bar.** The
		// element being dragged is not being *pressed* - a drag that lit up the
		// button it started on and activated it on release would make every
		// draggable button fire every time it was moved.
		if (pointer.Down && Detector != NULL_ENTITY) {
			const Vector2 point = dragPoint();
			ContinueDrag(store, point);
			Events.push_back(
				GuiEvent{
					EventKind::DragContinue,
					Detector,
					pointer.Position,
					Vector2{point.X - DragFrom.X, point.Y - DragFrom.Y},
					false,
					NULL_ENTITY,
					{},
					UINT32_MAX,
				}
			);
			Last = pointer.Position;
			WasDown = true;
			Started = true;
			return Events;
		}

		if (pointer.Down && !WasDown && pointer.Inside) {
			// **The bars are tested before the pick and win it.** A
			// `ScrollingFrame` is not `Active`, so the pick walks straight past
			// its bar to whatever is behind - which is right for the frame and
			// wrong for the chrome drawn on top of it. The thumbs come from
			// `ScrollState`, which is the same rectangle the compile drew, so
			// what you grab is what you see.
			if (Dragging == NULL_ENTITY) {
				const auto bar = FindPointerBar(store, list, pointer, [&](Entity entity, Entity) {
					return AllowsModal(store, entity);
				});
				if (bar.Target != NULL_ENTITY) {
					Dragging = bar.Target;
					DragVertical = bar.Vertical;
					DragGrab = bar.Grab;
				}
			}

			if (Dragging != NULL_ENTITY) {
				Last = pointer.Position;
				WasDown = true;
				Started = true;
				return Events;
			}

			// **After the bars and before the pick.** A scroll bar drawn over a
			// draggable panel is still the bar's, and a drag detector on a
			// `Frame` still wins over the press that would otherwise pass
			// straight through it.
			if (BeginDrag(store, list, pointer)) {
				Events.push_back(
					GuiEvent{
						EventKind::DragBegan,
						Detector,
						pointer.Position,
						Vector2::Zero,
						false,
						NULL_ENTITY,
						{},
						UINT32_MAX
					}
				);
				Last = pointer.Position;
				WasDown = true;
				Started = true;
				return Events;
			}

			// **Last of the three gestures, and gated on the pick finding
			// nothing.** A press that landed on something a script can react to
			// belongs to that thing: a button in a scrolling list stays
			// clickable, and only a press on the frame's own background is a
			// scroll. That ordering is why this sits below `BeginDrag` rather
			// than beside the bars, which win outright.
			if (found == NULL_ENTITY && BeginCanvasDrag(store, list, pointer)) {
				Last = pointer.Position;
				WasDown = true;
				Started = true;
				return Events;
			}

			Holding = found;
			HoldingCollection = NULL_ENTITY;
			HoldingKey.clear();
			HoldingIndex = UINT32_MAX;
			if (const DrawCommand *command = commandAt(found)) {
				HoldingCollection = command->Collection;
				HoldingKey = command->Key;
				HoldingIndex = command->Index;
			}
			if (store.Get<Entry>(found) != nullptr && HoldingCollection != NULL_ENTITY &&
				!HoldingKey.empty()) {
				(void)RememberVirtualFocus(store, HoldingCollection, HoldingKey, HoldingIndex);
			} else {
				ClearVirtualFocus(store);
			}
			emit(EventKind::InputBegan, found);

			// **A press that landed on nothing releases the focus, which is
			// Roblox's answer and the one a person expects**: clicking the
			// background is how anybody stops typing. The alternative - keeping
			// focus until some *other* box takes it - leaves a game with no way
			// to give the keyboard back to itself without adding a widget for the
			// purpose.
			//
			// **Only a press moves it.** A hover does not take the keyboard and
			// a release does not give it back, which is what makes dragging a
			// selection out of a box and letting go somewhere else keep the box
			// focused - the same interaction `InputEnded`'s rule protects one
			// paragraph up.
			//
			// **After `InputBegan`, because the press is the cause and the focus
			// change is what it did**, and release before capture for the reason
			// `MouseLeave` precedes `MouseEnter`: a handler putting state back
			// runs before the one reacting to the arrival.
			const Entity had = FocusedTextBox(store);
			const Entity wanted = store.Get<Entry>(found) != nullptr ? found : NULL_ENTITY;
			if (wanted != had && Focus(store, wanted)) {
				emit(EventKind::FocusReleased, had);
				emit(EventKind::Focused, wanted);
			}
			if (Entry *entry = store.GetMutable<Entry>(wanted); entry != nullptr) {
				if (const auto caret = FindPointerTextCaret(list, pointer, wanted)) {
					entry->CursorPosition = *caret;
					entry->SelectionStart = -1;
				}
			}
		} else if (!pointer.Down && WasDown) {
			// **Letting go of a bar ends the drag and nothing else.** No press
			// began, so no `InputEnded` is owed and no `Activated` can follow -
			// which is the same rule `Router::Forget` states from the other side:
			// firing at something nothing pressed is worse than firing nothing.
			Dragging = NULL_ENTITY;

			// Letting go of a canvas owes no event either, and starts the
			// rubber band coming back. Same rule, one gesture along.
			ReleaseCanvas(store);

			// A drag ends the same way, and it is the one of the two that owes
			// an event: a script watching `DragEnd` is what commits whatever the
			// drag was moving.
			if (Detector != NULL_ENTITY) {
				const Vector2 point = dragPoint();
				Events.push_back(
					GuiEvent{
						EventKind::DragEnded,
						Detector,
						pointer.Position,
						Vector2{point.X - DragFrom.X, point.Y - DragFrom.Y},
						false,
						NULL_ENTITY,
						{},
						UINT32_MAX,
					}
				);
				Detector = NULL_ENTITY;
				Dragged = NULL_ENTITY;
			}

			// **`InputEnded` goes to where the press began, not to where the
			// release happened.** That is what makes dragging off a button and
			// back one interaction rather than three, and it is the half people
			// get wrong: a release routed by position leaves the pressed button
			// stuck looking pressed.
			emit(EventKind::InputEnded, Holding);

			if (Holding != NULL_ENTITY && Holding == found) {
				emit(EventKind::Activated, Holding);
			}

			Holding = NULL_ENTITY;
			HoldingCollection = NULL_ENTITY;
			HoldingKey.clear();
			HoldingIndex = UINT32_MAX;
		}

		// **After the press, so a wheel turned in the same frame as a click acts
		// on the thing the click landed on.** They arrive together often enough -
		// a trackpad reports both - and the other order scrolls the list away
		// from under the button before it is pressed.
		if (pointer.Wheel != 0.0f && pointer.Inside) {
			Wheel(store, list, pointer);
		}

		Last = pointer.Position;
		WasDown = pointer.Down;
		Started = true;

		return Events;
	}
	// Compare prospective semantic effects before routing delayed input.
	bool Router::MatchesSemanticFrame(
		Store &store,
		const DrawList &list,
		const PointerFrame &frame,
		const RouterCaptureWitness &continuity,
		uint64_t routerIncarnation,
		SemanticAction action
	) const {
		if (!frame.Valid() || frame.State->StoreIdentity != store.Identity() ||
			!MatchesRouterContinuity(store, continuity, routerIncarnation))
			return false;
		try {
			const auto preview = PreviewModals(store, list);
			const auto &facts = *frame.State;
			if (preview.Overflow || facts.ModalOverflow) return false;
			size_t modalCount = 0;
			for (size_t i = 0; i < preview.Count; ++i) {
				const auto &c = preview.Contexts[i];
				if (c.NextModal == NULL_ENTITY) continue;
				++modalCount;
				const auto found = std::find_if(facts.Modals.begin(), facts.Modals.end(), [&](const auto &m) {
					return m.Collector == c.Collector && m.Next == c.NextModal;
				});
				if (found == facts.Modals.end()) return false;
			}
			if (modalCount != facts.Modals.size()) return false;
			const auto collector = [&](Entity e) { return CollectorOf(store, e); };
			struct ProjectAccess {
				const Store &World;
				const detail::PointerFrameFacts &Facts;
				const ModalPreview &Preview;
				const decltype(collector) &CollectorFn;
				bool Frozen;
				bool Saved(Entity e) const {
					for (size_t i = 0; i < Preview.Count; ++i)
						if (Preview.Contexts[i].FocusBeforeModal == e ||
							Preview.Contexts[i].SelectionBeforeModal == e)
							return true;
					return false;
				}
				bool Known(Entity e) const {
					return !Frozen || Saved(e) || Facts.Find(e) != nullptr;
				}
				bool Alive(Entity e) const {
					const auto *n = Facts.Find(e);
					return !Frozen || Saved(e) ? World.Alive(e) : n && n->Alive;
				}
				Entity Collector(Entity e) const {
					return !Frozen || Saved(e) ? CollectorFn(e) : Facts.Collector(e);
				}
				bool Inside(Entity e, Entity top) const {
					return !Frozen ? (e == top || World.IsDescendantOf(e, top)) : Facts.IsDescendant(e, top);
				}
				bool IsEntry(Entity e) const {
					return !Frozen || Saved(e) ? World.Get<Entry>(e) != nullptr : Facts.IsEntry(e);
				}
				bool Selectable(Entity e) const {
					const auto *v = (!Frozen || Saved(e)) ? World.Get<Element>(e) : Facts.ElementOf(e);
					return v && v->Visible && v->Selectable && v->Interactable;
				}
			};
			const ProjectAccess live{store, facts, preview, collector, false},
				frozen{store, facts, preview, collector, true};
			const auto *state = store.Get<GuiServiceState>(GuiServiceOf(store));
			const Entity selected = state ? state->SelectedObject : NULL_ENTITY;
			const bool autoSelect = state && state->AutoSelectGuiEnabled;
			const Entity focused = FocusedTextBox(store);
			const auto contexts = std::span(preview.Contexts.data(), preview.Count);
			const auto expected =
				detail::ProjectModalTransition(frozen, contexts, focused, selected, state != nullptr);
			const auto actual =
				detail::ProjectModalTransition(live, contexts, focused, selected, state != nullptr);
			if (!expected.Valid || !actual.Valid || expected.Focus != actual.Focus ||
				expected.Selection != actual.Selection || expected.FocusCount != actual.FocusCount)
				return false;
			for (size_t i = 0; i < expected.FocusCount; ++i)
				if (expected.FocusTargets[i] != actual.FocusTargets[i]) return false;
			// Modal opening can cancel any capture in that collector. Owner collector
			// identity must also be present in the retained frame.
			const std::array<Entity, 5> captures{Holding, Dragging, Detector, Dragged, Canvas};
			for (size_t i = 0; i < preview.Count; ++i) {
				const auto &c = preview.Contexts[i];
				if (c.Modal == c.NextModal || c.NextModal == NULL_ENTITY) continue;
				for (const Entity owner : captures) {
					if (owner == NULL_ENTITY) continue;
					if (!facts.Find(owner)) return false;
					const Entity currentCollector = collector(owner),
								 displayedCollector = facts.Collector(owner);
					if ((currentCollector == c.Collector || displayedCollector == c.Collector) &&
						currentCollector != displayedCollector)
						return false;
				}
			}
			if (action == SemanticAction::Cancel) return true;
			struct Scope {
				Entity Collector, Modal;
				bool Ambiguous = false;
			};
			const auto scope = [&](const ProjectAccess &a, Entity current) {
				Scope out;
				out.Collector = a.Collector(current);
				if (out.Collector == NULL_ENTITY) {
					for (size_t i = 0; i < preview.Count; ++i)
						if (preview.Contexts[i].NextModal != NULL_ENTITY) {
							if (out.Collector != NULL_ENTITY) {
								out.Collector = {};
								out.Ambiguous = true;
								break;
							}
							out.Collector = preview.Contexts[i].Collector;
						}
				}
				for (size_t i = 0; i < preview.Count; ++i)
					if (preview.Contexts[i].Collector == out.Collector) {
						out.Modal = preview.Contexts[i].NextModal;
						break;
					}
				return out;
			};
			const Scope a = scope(live, actual.Selection), b = scope(frozen, expected.Selection);
			if (a.Collector != b.Collector || a.Modal != b.Modal || a.Ambiguous != b.Ambiguous) return false;
			SelectionMove move = SelectionMove::Up;
			bool navigate = true;
			switch (action) {
			case SemanticAction::Up:
				break;
			case SemanticAction::Down:
				move = SelectionMove::Down;
				break;
			case SemanticAction::Left:
				move = SelectionMove::Left;
				break;
			case SemanticAction::Right:
				move = SelectionMove::Right;
				break;
			default:
				navigate = false;
				break;
			}
			if (navigate) {
				if (a.Ambiguous || !state) return true;
				std::vector<const DrawCommand *> liveCommands;
				std::vector<const detail::PointerFrameCommand *> frameCommands;
				for (const auto &cmd : list.Commands)
					if (a.Collector == NULL_ENTITY ||
						(cmd.Collector == a.Collector && (a.Modal == NULL_ENTITY || cmd.Source == a.Modal ||
														  store.IsDescendantOf(cmd.Source, a.Modal))))
						liveCommands.push_back(&cmd);
				for (const auto &cmd : facts.Commands)
					if (b.Collector == NULL_ENTITY ||
						(cmd.Collector == b.Collector &&
						 (b.Modal == NULL_ENTITY || facts.IsDescendant(cmd.Source, b.Modal))))
						frameCommands.push_back(&cmd);
				struct LiveSource {
					const Store &World;
					const Element *ElementOf(Entity e) const {
						return World.Get<Element>(e);
					}
					const Selection *SelectionOf(Entity e) const {
						return World.Get<Selection>(e);
					}
				};
				const LiveSource source{store};
				const detail::NavigationAccess<DrawCommand, LiveSource> la{liveCommands, source};
				const detail::NavigationAccess<detail::PointerFrameCommand, detail::PointerFrameFacts> fa{
					frameCommands, facts
				};
				return detail::ChooseNavigation(la, actual.Selection, autoSelect, move) ==
					   detail::ChooseNavigation(fa, expected.Selection, autoSelect, move);
			}
			// The untargeted overload routes both Activate and Focus through this same
			// button activation branch. Its compiled check does not require clipping.
			const Entity target = actual.Selection;
			const auto liveElement = store.Get<Element>(target);
			const auto *node = facts.Find(target);
			const bool liveCompiled =
				std::any_of(list.Commands.begin(), list.Commands.end(), [&](const auto &c) {
					return c.Source == target;
				});
			const bool frozenCompiled =
				std::any_of(facts.Commands.begin(), facts.Commands.end(), [&](const auto &c) {
					return c.Source == target;
				});
			const auto liveAllowed =
				target != NULL_ENTITY &&
				(a.Modal == NULL_ENTITY || target == a.Modal || store.IsDescendantOf(target, a.Modal)) &&
				store.IsA(target, ButtonClass()) && liveElement && liveElement->Visible &&
				liveElement->Selectable && liveElement->Interactable && liveCompiled;
			const auto *fe = node && node->ElementValue ? &*node->ElementValue : nullptr;
			const auto frozenAllowed = target != NULL_ENTITY &&
									   (b.Modal == NULL_ENTITY || facts.IsDescendant(target, b.Modal)) &&
									   node && node->GuiButton && fe && fe->Visible && fe->Selectable &&
									   fe->Interactable && frozenCompiled;
			return liveAllowed == frozenAllowed;
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	// The shared check validates prospective modal effects before target eligibility.
	bool Router::MatchesTargetedSemanticFrame(
		Store &store,
		const DrawList &list,
		const PointerFrame &frame,
		const RouterCaptureWitness &continuity,
		uint64_t routerIncarnation,
		Entity target,
		Entity expectedCollector,
		SemanticAction action
	) const {
		if (!MatchesSemanticFrame(store, list, frame, continuity, routerIncarnation, SemanticAction::Cancel))
			return false;
		try {
			const auto preview = PreviewModals(store, list);
			Entity liveModal;
			for (size_t i = 0; i < preview.Count; ++i)
				if (preview.Contexts[i].Collector == expectedCollector) {
					liveModal = preview.Contexts[i].NextModal;
					break;
				}
			Entity frameModal;
			for (const auto &modal : frame.State->Modals)
				if (modal.Collector == expectedCollector) {
					frameModal = modal.Next;
					break;
				}
			const bool liveAllowed =
				liveModal == NULL_ENTITY || target == liveModal || store.IsDescendantOf(target, liveModal);
			const bool frameAllowed =
				frameModal == NULL_ENTITY || frame.State->IsDescendant(target, frameModal);
			return detail::MatchesTargetedSemantic(
				store,
				list,
				*frame.State,
				target,
				expectedCollector,
				action,
				liveAllowed,
				frameAllowed,
				[&](Entity e) { return CollectorOf(store, e); },
				[&](Entity e) { return store.IsA(e, ButtonClass()); }
			);
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	bool
	Router::MatchesTypingFrame(const Store &store, const DrawList &list, const PointerFrame &frame) const {
		if (!frame.Valid()) return false;
		return detail::MatchesTypingFrame(store, list, *frame.State, [&](Entity e) {
			return CollectorOf(store, e);
		});
	}

	bool Router::MatchesCapturedCanvasFrame(
		Store &store,
		const DrawList &list,
		const PointerFrame &frame,
		const Pointer &pointer,
		const RouterCaptureWitness &continuity,
		uint64_t routerIncarnation
	) const {
		if (!frame.Valid() || frame.State->StoreIdentity != store.Identity() || Canvas == NULL_ENTITY ||
			Holding != NULL_ENTITY || Dragging != NULL_ENTITY || Detector != NULL_ENTITY ||
			Dragged != NULL_ENTITY || !WasDown || !pointer.Down || pointer.Cancelled ||
			pointer.Wheel != 0.0f || !MatchesRouterContinuity(store, continuity, routerIncarnation))
			return false;
		try {
			const auto preview = PreviewModals(store, list);
			if (preview.Overflow) return false;
			for (size_t i = 0; i < preview.Count; ++i)
				if (preview.Contexts[i].Modal != preview.Contexts[i].NextModal) return false;
			const auto saved = detail::SamplePointerFrameFacts(*frame.State, pointer);
			const auto current = DecidePointer(store, list, pointer);
			if (!saved.Valid || !current.Valid || saved.Hovered != current.Hovered ||
				saved.Hits != current.Hits || saved.ModalOverflow != current.ModalOverflow ||
				saved.Modals.size() != current.Modals.size())
				return false;
			for (size_t i = 0; i < saved.Modals.size(); ++i)
				if (saved.Modals[i].Collector != current.Modals[i].Collector ||
					saved.Modals[i].Next != current.Modals[i].Next)
					return false;
			const auto sameLocal = [&](Entity e) {
				if (e == NULL_ENTITY) return true;
				const auto *node = frame.State->Find(e);
				if (!node || node->Alive != store.Alive(e)) return false;
				const auto *resolved = store.Get<Resolved>(e);
				if ((resolved != nullptr) != node->ResolvedValue.has_value()) return false;
				if (!resolved) return true;
				if (resolved->AbsolutePosition != node->ResolvedValue->AbsolutePosition) return false;
				Entity liveCollector, savedCollector;
				for (auto it = list.Commands.rbegin(); it != list.Commands.rend(); ++it)
					if (it->Source == e) {
						liveCollector = it->Collector;
						break;
					}
				for (auto it = frame.State->Commands.rbegin(); it != frame.State->Commands.rend(); ++it)
					if (it->Source == e) {
						savedCollector = it->Collector;
						break;
					}
				return liveCollector == savedCollector &&
					   LogicalPoint(list, liveCollector, pointer.Position) ==
						   detail::PointerLogical(*frame.State, savedCollector, pointer.Position);
			};
			if (!sameLocal(Over) || !sameLocal(current.Hovered)) return false;
			const auto *canvas = frame.State->Find(Canvas);
			const auto *scrolling = store.Get<Scrolling>(Canvas);
			const auto *state = store.Get<ScrollState>(Canvas);
			const auto *motion = store.Get<ScrollMotion>(Canvas);
			const Entity currentCollector = CollectorOf(store, Canvas);
			if (!canvas || !canvas->ScrollingValue || !canvas->ScrollValue || !scrolling || !state ||
				!motion || !canvas->Alive || !store.Alive(Canvas) || currentCollector == NULL_ENTITY ||
				canvas->Collector != currentCollector ||
				scrolling->CanvasPosition != canvas->ScrollingValue->CanvasPosition ||
				scrolling->Enabled != canvas->ScrollingValue->Enabled ||
				scrolling->Direction != canvas->ScrollingValue->Direction ||
				scrolling->Elastic != canvas->ScrollingValue->Elastic ||
				state->CanvasSize != canvas->ScrollValue->CanvasSize ||
				state->WindowSize != canvas->ScrollValue->WindowSize)
				return false;
			const ScrollMotion before = canvas->MotionValue.value_or(ScrollMotion{});
			if (!motion->Held || motion->ReleasedAt != -1.0 || motion->Pull != before.Overshoot ||
				motion->Released != Vector2{} || motion->Overshoot != before.Overshoot ||
				motion->LastPoint != LogicalPoint(list, currentCollector, Last))
				return false;
			return (pointer.Collector == currentCollector ||
					(pointer.Collector == NULL_ENTITY && pointer.ScreenOnly &&
					 !frame.State->IsSpatial(currentCollector))) &&
				   LogicalPoint(list, currentCollector, pointer.Position) ==
					   detail::PointerLogical(*frame.State, canvas->Collector, pointer.Position);
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	bool Router::MatchesCapturedDetectorFrame(
		Store &store,
		const DrawList &list,
		const PointerFrame &frame,
		const Pointer &pointer,
		const RouterCaptureWitness &continuity,
		uint64_t routerIncarnation
	) const {
		if (!frame.Valid() || frame.State->StoreIdentity != store.Identity() || Detector == NULL_ENTITY ||
			Dragged == NULL_ENTITY || Holding != NULL_ENTITY || Dragging != NULL_ENTITY ||
			Canvas != NULL_ENTITY || !WasDown || pointer.Cancelled || pointer.Wheel != 0.0f ||
			!MatchesRouterContinuity(store, continuity, routerIncarnation))
			return false;

		try {
			const auto preview = PreviewModals(store, list);
			if (preview.Overflow) return false;
			for (size_t i = 0; i < preview.Count; ++i)
				if (preview.Contexts[i].Modal != preview.Contexts[i].NextModal) return false;
			const auto saved = detail::SamplePointerFrameFacts(*frame.State, pointer);
			const auto current = DecidePointer(store, list, pointer);
			if (!saved.Valid || !current.Valid || saved.ModalOverflow != current.ModalOverflow ||
				saved.Modals.size() != current.Modals.size())
				return false;
			// Without a causal mutation receipt, moved hit geometry cannot be
			// distinguished from an unrelated authored write.
			if (saved.Hits != current.Hits || saved.Hovered != current.Hovered) return false;
			for (size_t i = 0; i < saved.Modals.size(); ++i)
				if (saved.Modals[i].Collector != current.Modals[i].Collector ||
					saved.Modals[i].Next != current.Modals[i].Next)
					return false;
			const auto sameLocal = [&](Entity e) {
				if (e == NULL_ENTITY) return true;
				const auto *node = frame.State->Find(e);
				if (!node || node->Alive != store.Alive(e)) return false;
				const auto *resolved = store.Get<Resolved>(e);
				if ((resolved != nullptr) != node->ResolvedValue.has_value()) return false;
				if (!resolved) return true;
				if (resolved->AbsolutePosition != node->ResolvedValue->AbsolutePosition) return false;
				Entity liveCollector, savedCollector;
				for (auto it = list.Commands.rbegin(); it != list.Commands.rend(); ++it)
					if (it->Source == e) {
						liveCollector = it->Collector;
						break;
					}
				for (auto it = frame.State->Commands.rbegin(); it != frame.State->Commands.rend(); ++it)
					if (it->Source == e) {
						savedCollector = it->Collector;
						break;
					}
				return liveCollector == savedCollector &&
					   LogicalPoint(list, liveCollector, pointer.Position) ==
						   detail::PointerLogical(*frame.State, savedCollector, pointer.Position);
			};
			if (!sameLocal(Over) || !sameLocal(current.Hovered)) return false;
			const auto *dragged = frame.State->Find(Dragged);
			const auto *element = store.Get<Element>(Dragged);
			const auto *resolved = store.Get<Resolved>(Dragged);
			const auto *detector = store.Get<DragDetector>(Detector);
			if (!dragged || dragged->Detector != Detector || !dragged->DetectorValue || !detector ||
				!dragged->ElementValue || !dragged->ResolvedValue || !element || !resolved ||
				element->Position != dragged->ElementValue->Position ||
				element->Rotation != dragged->ElementValue->Rotation ||
				resolved->AbsolutePosition != dragged->ResolvedValue->AbsolutePosition ||
				resolved->AbsoluteSize != dragged->ResolvedValue->AbsoluteSize ||
				resolved->AbsoluteRotation != dragged->ResolvedValue->AbsoluteRotation ||
				resolved->Clip != dragged->ResolvedValue->Clip)
				return false;
			const auto &oldDetector = *dragged->DetectorValue;
			if (detector->BoundingUI != oldDetector.BoundingUI || detector->Axis != oldDetector.Axis ||
				detector->MinTranslation != oldDetector.MinTranslation ||
				detector->MaxTranslation != oldDetector.MaxTranslation ||
				detector->Style != oldDetector.Style || detector->Response != oldDetector.Response ||
				detector->Enabled != oldDetector.Enabled || store.ParentOf(Dragged) != dragged->Parent)
				return false;
			const auto sameBounds = [&](Entity entity) {
				if (entity == NULL_ENTITY) return true;
				const auto *old = frame.State->Find(entity);
				if (!old || old->Alive != store.Alive(entity)) return false;
				const auto *now = store.Get<Resolved>(entity);
				if ((now != nullptr) != old->ResolvedValue.has_value()) return false;
				return !now || (now->AbsolutePosition == old->ResolvedValue->AbsolutePosition &&
								now->AbsoluteSize == old->ResolvedValue->AbsoluteSize);
			};
			if (!sameBounds(dragged->Parent) || !sameBounds(detector->BoundingUI)) return false;
			const Entity collector = CollectorOf(store, Dragged);
			return dragged && dragged->Alive && store.Alive(Dragged) && store.Alive(Detector) &&
				   store.Get<DragDetector>(Detector) && store.Get<Element>(Dragged) &&
				   store.Get<Resolved>(Dragged) && store.IsDescendantOf(Detector, Dragged) &&
				   collector != NULL_ENTITY && dragged->Collector == collector &&
				   CollectorOf(store, Detector) == collector &&
				   (pointer.Collector == collector ||
					(pointer.Collector == NULL_ENTITY && pointer.ScreenOnly &&
					 !frame.State->IsSpatial(collector))) &&
				   LogicalPoint(list, collector, pointer.Position) ==
					   detail::PointerLogical(*frame.State, collector, pointer.Position);
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

	bool Router::MatchesIdlePointerFrame(
		Store &store,
		const DrawList &list,
		const PointerFrame &frame,
		const Pointer &pointer,
		const RouterCaptureWitness &continuity,
		uint64_t routerIncarnation
	) const {
		if (!frame.Valid() || frame.State->StoreIdentity != store.Identity() || !Started || WasDown ||
			pointer.Down || pointer.Cancelled || pointer.Wheel != 0.0f || Holding != NULL_ENTITY ||
			Dragging != NULL_ENTITY || Detector != NULL_ENTITY || Dragged != NULL_ENTITY ||
			Canvas != NULL_ENTITY || !MatchesRouterContinuity(store, continuity, routerIncarnation))
			return false;
		try {
			const auto preview = PreviewModals(store, list);
			if (preview.Overflow) return false;
			for (size_t i = 0; i < preview.Count; ++i)
				if (preview.Contexts[i].Modal != preview.Contexts[i].NextModal) return false;
			const auto saved = detail::SamplePointerFrameFacts(*frame.State, pointer);
			const auto current = DecidePointer(store, list, pointer);
			if (!saved.Valid || !current.Valid || saved.Hovered != Over || current.Hovered != Over ||
				saved.ModalOverflow != current.ModalOverflow || saved.Modals != current.Modals)
				return false;
			if (pointer.Position == Last || Over == NULL_ENTITY) return true;
			if (saved.Hits != current.Hits) return false;
			const auto *node = frame.State->Find(Over);
			const auto *resolved = store.Get<Resolved>(Over);
			if (!node || node->Alive != store.Alive(Over) ||
				(resolved != nullptr) != node->ResolvedValue.has_value())
				return false;
			if (!resolved) return true;
			if (resolved->AbsolutePosition != node->ResolvedValue->AbsolutePosition) return false;
			Entity liveCollector, savedCollector;
			for (auto it = list.Commands.rbegin(); it != list.Commands.rend(); ++it)
				if (it->Source == Over) {
					liveCollector = it->Collector;
					break;
				}
			for (auto it = frame.State->Commands.rbegin(); it != frame.State->Commands.rend(); ++it)
				if (it->Source == Over) {
					savedCollector = it->Collector;
					break;
				}
			return liveCollector == savedCollector &&
				   LogicalPoint(list, liveCollector, pointer.Position) ==
					   detail::PointerLogical(*frame.State, savedCollector, pointer.Position);
		} catch (const std::bad_alloc &) {
			return false;
		}
	}

}
