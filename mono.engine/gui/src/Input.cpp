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

		// Whether an element takes input rather than passing it through.
		//
		// A `GuiButton` does whatever its `Active` says, which is Roblox's
		// rule and is why the two tests are an `or` rather than one test on a
		// field the button constructor sets. Setting the field instead would
		// let a script clear it and produce a button that cannot be clicked
		// with nothing in the tree explaining why.
		//
		// **A `TextBox` is the second class that does, and it had to be.** A
		// press is the only gesture that decides where typing goes, so a box the
		// pick walked *past* could never be focused - clicking one landed on
		// whatever was behind it, which is the state this engine shipped in and
		// which reads as a text field that ignores the mouse. `Entry` is the
		// test for the same reason `Element` is the `GuiObject` test in
		// `ElementsAt`: the component is on that class and on no other.
		bool TakesInput(const Store &store, Entity instance) {
			if (store.IsA(instance, ButtonClass()) || store.Get<Entry>(instance) != nullptr) {
				return true;
			}
			const Element *element = store.Get<Element>(instance);
			return element != nullptr && element->Active;
		}
	}

	Entity Pick(const Store &store, const DrawList &list, const Vector2 &point) {
		ENGINE_PROFILE_CAT("gui pick", engine::core::ProfileCategory::ECS);

		// Backwards, which is front to back. `DrawList`'s own comment says the
		// list is back-to-front for exactly this reason.
		for (size_t index = list.Commands.size(); index > 0; index--) {
			const DrawCommand &command = list.Commands[index - 1];

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

			if (TakesInput(store, command.Source)) {
				return command.Source;
			}

			// Not `break`. An inactive element is transparent to input, so the
			// walk carries on to whatever is behind it - which is what lets a
			// background panel exist without swallowing the interface it
			// contains.
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

	std::span<const GuiEvent> Router::Update(Store &store, const DrawList &list, const Pointer &pointer) {
		ENGINE_PROFILE_CAT("gui route", engine::core::ProfileCategory::ECS);

		Events.clear();

		const Entity found = pointer.Inside ? Pick(store, list, pointer.Position) : NULL_ENTITY;

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

		if (pointer.Down && !WasDown) {
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

		Last = pointer.Position;
		WasDown = pointer.Down;
		Started = true;

		return Events;
	}
}
