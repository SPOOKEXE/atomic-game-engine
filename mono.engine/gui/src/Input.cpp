#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>

#include <algorithm>
#include <cmath>
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
							   (element != nullptr && element->Active);
			if (!takes) {
				return Reach::Through;
			}

			return element == nullptr || element->Interactable ? Reach::Hit : Reach::Blocked;
		}
	}

	namespace {
		Entity PickWhere(
			const Store &store, const DrawList &list, const Vector2 &point, Entity collector, bool screenOnly
		) {
			ENGINE_PROFILE_CAT("gui pick", engine::core::ProfileCategory::ECS);

			// Backwards, which is front to back. `DrawList`'s own comment says the
			// list is back-to-front for exactly this reason.
			for (size_t index = list.Commands.size(); index > 0; index--) {
				const DrawCommand &command = list.Commands[index - 1];
				if (collector != NULL_ENTITY && command.Collector != collector) {
					continue;
				}
				if (screenOnly && store.Get<SpatialCanvas>(command.Collector) != nullptr) {
					continue;
				}

				// **The clip is tested as well as the bounds.** An element scrolled
				// out of its parent still has a rectangle - `Resolved` keeps it
				// deliberately - and clicking where it would have been must not
				// find it.
				// **The point is turned into the element's own space, not the
				// rectangle into the screen's.** A rotated rectangle is not a
				// rectangle and testing one needs a polygon; rotating the *point*
				// back by the same angle makes the test the axis-aligned one it
				// already was, exactly.
				//
				// This was `D00025`: `Bounds` is the unrotated rectangle and
				// `Rotation` sat beside it unread, so a rotated button drew in one
				// place and answered a pointer in another - the kind of bug people
				// file twice, once against the drawing and once against the input.
				//
				// **The clip is deliberately not rotated.** A clip is a scissor
				// rectangle and a scissor is axis-aligned on every backend there is,
				// so an element rotated inside a clipped container is still cut by
				// an upright rectangle - which is what the painter does and what the
				// hit test therefore has to agree with.
				const core::Vector2 local = Unrotated(command.Rotation, command.Bounds, point);
				if (!command.Bounds.Contains(local) || !command.Clip.Contains(point)) {
					continue;
				}

				switch (Reaches(store, command.Source)) {
				case Reach::Hit:
					return command.Source;
				case Reach::Blocked:
					return NULL_ENTITY;
				case Reach::Through:
					break;
				}

				// Not `break` out of the loop. An inactive element is transparent
				// to input, so the walk carries on to whatever is behind it -
				// which is what lets a background panel exist without swallowing
				// the interface it contains.
			}

			return NULL_ENTITY;
		}
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

	Entity Router::Wheel(Store &store, const Vector2 &point, float notches) {
		// **The frames are asked directly rather than through `Pick`.** A
		// `ScrollingFrame` is not `Active` and usually holds nothing that is, so
		// the pick walks straight past it and answers null - and a wheel that
		// only worked while the pointer happened to be over a button would be
		// one nobody could use. A frame may also be perfectly invisible, so its
		// *rectangle* is the question rather than anything it drew.
		//
		// **Innermost first, by the same key `ElementsAt` sorts on.** A list
		// inside a list takes the wheel until it reaches its own end and then
		// stops, rather than handing the remainder to the page behind it -
		// Roblox's behaviour, and the one that stops a nested panel dragging its
		// background around.
		Entity best;
		int32_t bestOrder = 0;
		int32_t bestDepth = 0;

		store.Each<const Scrolling, const ScrollState, const Resolved>(
			[&](Entity node, const Scrolling &scrolling, const ScrollState &state, const Resolved &resolved) {
				if (!resolved.Rendered ||
					(!Scrolls(scrolling, state, true) && !Scrolls(scrolling, state, false))) {
					return;
				}

				const core::Rect bounds{
					resolved.AbsolutePosition,
					Vector2{
						resolved.AbsolutePosition.X + resolved.AbsoluteSize.X,
						resolved.AbsolutePosition.Y + resolved.AbsoluteSize.Y,
					},
				};

				if (!bounds.Contains(Unrotated(resolved.AbsoluteRotation, bounds, point)) ||
					!resolved.Clip.Contains(point)) {
					return;
				}

				const bool wins = best == NULL_ENTITY || resolved.Order > bestOrder ||
								  (resolved.Order == bestOrder && resolved.Depth > bestDepth);
				if (!wins) {
					return;
				}

				best = node;
				bestOrder = resolved.Order;
				bestDepth = resolved.Depth;
			}
		);

		if (best == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		const ScrollState *state = store.Get<ScrollState>(best);
		Scrolling *writable = store.GetMutable<Scrolling>(best);
		if (state == nullptr || writable == nullptr) {
			return NULL_ENTITY;
		}

		// **Vertical first, because a frame that scrolls both is a page and a
		// wheel on a page means down.** A horizontal-only frame takes the same
		// turn sideways, which is the only reading that leaves the wheel useful
		// on one.
		//
		// Negated, because a turn away from the person moves the canvas back
		// towards its start - `Pointer::Wheel` carries the argument.
		if (Scrolls(*writable, *state, true)) {
			Move(*writable, *state, 0.0f, -notches * WHEEL_PIXELS);
		} else {
			Move(*writable, *state, -notches * WHEEL_PIXELS, 0.0f);
		}
		return best;
	}

	bool Router::BeginDrag(Store &store, const DrawList &list, const Vector2 &point) {
		// **Front to back over the list, and every element counts.** A drag
		// detector makes its parent draggable whatever that parent's `Active`
		// says - a decorative `Frame` with one on it is exactly the ordinary use
		// - so this is the list walk rather than `Pick`, which would have walked
		// past it.
		for (size_t index = list.Commands.size(); index > 0; index--) {
			const DrawCommand &command = list.Commands[index - 1];
			if (!command.Bounds.Contains(Unrotated(command.Rotation, command.Bounds, point)) ||
				!command.Clip.Contains(point)) {
				continue;
			}

			const Element *element = store.Get<Element>(command.Source);
			if (element == nullptr) {
				continue;
			}

			// **The first enabled detector among the children wins**, which is
			// the same rule `Emit` applies to a `UIStroke` and a `UICorner`. Two
			// on one element is an authoring mistake with no sensible reading.
			Entity found;
			store.EachChild(command.Source, [&](Entity child) {
				if (found != NULL_ENTITY) {
					return;
				}
				if (const DragDetector *detector = store.Get<DragDetector>(child);
					detector != nullptr && detector->Enabled) {
					found = child;
				}
			});

			if (found == NULL_ENTITY) {
				continue;
			}

			Detector = found;
			Dragged = command.Source;
			DragFrom = point;
			DragStart = element->Position;
			DragAngle = element->Rotation;
			return true;
		}

		return false;
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

	bool Router::BeginCanvasDrag(Store &store, const DrawList &list, const Vector2 &point) {
		// Topmost first, which is the same walk the bars take and for the same
		// reason: a frame drawn over another frame gets the press.
		for (size_t index = list.Commands.size(); index > 0; index--) {
			const DrawCommand &command = list.Commands[index - 1];

			const Scrolling *scrolling = store.Get<Scrolling>(command.Source);
			const ScrollState *state = store.Get<ScrollState>(command.Source);
			if (scrolling == nullptr || state == nullptr || !scrolling->Enabled ||
				!command.Clip.Contains(point)) {
				continue;
			}

			// **A frame that cannot move and cannot stretch is not holding
			// anything.** Taking the press anyway would swallow it from
			// whatever is behind, which for a list that happens to fit its
			// window is every press in it.
			const bool movesY = Scrolls(*scrolling, *state, true) || Elastic(*scrolling, *state, true);
			const bool movesX = Scrolls(*scrolling, *state, false) || Elastic(*scrolling, *state, false);
			if (!movesX && !movesY) {
				continue;
			}

			Canvas = command.Source;

			ScrollMotion motion;
			if (const ScrollMotion *had = store.Get<ScrollMotion>(Canvas)) {
				motion = *had;
			}

			// **The pull carries over from a spring still in flight**, so
			// catching a bouncing list mid-return takes hold of it where it is
			// rather than snapping it back to the end first.
			motion.Pull = motion.Overshoot;
			motion.Released = core::Vector2{};
			motion.ReleasedAt = -1.0;
			motion.LastPoint = point;
			motion.Held = true;
			store.Set(Canvas, motion);
			return true;
		}
		return false;
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

		const float origin = DragVertical ? resolved->AbsolutePosition.Y : resolved->AbsolutePosition.X;
		const float at = (DragVertical ? point.Y : point.X) - origin - DragGrab;
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

	std::span<const GuiEvent> Router::Update(Store &store, const DrawList &list, const Pointer &pointer) {
		ENGINE_PROFILE_CAT("gui route", engine::core::ProfileCategory::ECS);

		Events.clear();

		const Entity found = !pointer.Inside ? NULL_ENTITY
							 : pointer.Collector != NULL_ENTITY
								 ? PickInCollector(store, list, pointer.Collector, pointer.Position)
							 : pointer.ScreenOnly ? PickScreen(store, list, pointer.Position)
												  : Pick(store, list, pointer.Position);

		const auto local = [&](Entity instance) -> Vector2 {
			const Resolved *resolved = store.Get<Resolved>(instance);
			if (resolved == nullptr) {
				return Vector2::Zero;
			}
			return Vector2{
				pointer.Position.X - resolved->AbsolutePosition.X,
				pointer.Position.Y - resolved->AbsolutePosition.Y,
			};
		};

		const auto emit = [&](EventKind kind, Entity instance) {
			if (instance != NULL_ENTITY) {
				Events.push_back(GuiEvent{kind, instance, pointer.Position, local(instance)});
			}
		};

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
			DragBar(store, pointer.Position);
			Last = pointer.Position;
			WasDown = true;
			Started = true;
			return Events;
		}

		// **A held canvas, on the same footing as a held bar**, and before the
		// detector for the reason the bar is before both: a gesture already in
		// flight owns the pointer until it is let go of.
		if (pointer.Down && Canvas != NULL_ENTITY) {
			DragCanvas(store, pointer.Position);
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
			ContinueDrag(store, pointer.Position);
			Events.push_back(
				GuiEvent{
					EventKind::DragContinue,
					Detector,
					pointer.Position,
					Vector2{pointer.Position.X - DragFrom.X, pointer.Position.Y - DragFrom.Y},
				}
			);
			Last = pointer.Position;
			WasDown = true;
			Started = true;
			return Events;
		}

		if (pointer.Down && !WasDown) {
			// **The bars are tested before the pick and win it.** A
			// `ScrollingFrame` is not `Active`, so the pick walks straight past
			// its bar to whatever is behind - which is right for the frame and
			// wrong for the chrome drawn on top of it. The thumbs come from
			// `ScrollState`, which is the same rectangle the compile drew, so
			// what you grab is what you see.
			for (size_t index = list.Commands.size(); index > 0 && Dragging == NULL_ENTITY; index--) {
				const DrawCommand &command = list.Commands[index - 1];
				if (pointer.Collector != NULL_ENTITY && command.Collector != pointer.Collector) {
					continue;
				}

				const Scrolling *scrolling = store.Get<Scrolling>(command.Source);
				const ScrollState *state = store.Get<ScrollState>(command.Source);
				const Resolved *resolved = store.Get<Resolved>(command.Source);
				if (scrolling == nullptr || state == nullptr || resolved == nullptr || !scrolling->Enabled ||
					!command.Clip.Contains(pointer.Position)) {
					continue;
				}

				if (state->VerticalThumb.Contains(pointer.Position)) {
					Dragging = command.Source;
					DragVertical = true;
					DragGrab = pointer.Position.Y - state->VerticalThumb.Min.Y;
				} else if (state->HorizontalThumb.Contains(pointer.Position)) {
					Dragging = command.Source;
					DragVertical = false;
					DragGrab = pointer.Position.X - state->HorizontalThumb.Min.X;
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
			if (BeginDrag(store, list, pointer.Position)) {
				Events.push_back(GuiEvent{EventKind::DragBegan, Detector, pointer.Position, Vector2::Zero});
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
			if (found == NULL_ENTITY && BeginCanvasDrag(store, list, pointer.Position)) {
				Last = pointer.Position;
				WasDown = true;
				Started = true;
				return Events;
			}

			Holding = found;
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
				Events.push_back(
					GuiEvent{
						EventKind::DragEnded,
						Detector,
						pointer.Position,
						Vector2{pointer.Position.X - DragFrom.X, pointer.Position.Y - DragFrom.Y},
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
		}

		// **After the press, so a wheel turned in the same frame as a click acts
		// on the thing the click landed on.** They arrive together often enough -
		// a trackpad reports both - and the other order scrolls the list away
		// from under the button before it is pressed.
		if (pointer.Wheel != 0.0f && pointer.Inside) {
			Wheel(store, pointer.Position, pointer.Wheel);
		}

		Last = pointer.Position;
		WasDown = pointer.Down;
		Started = true;

		return Events;
	}
}
