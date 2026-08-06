#include <engine/core/Profiling.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Input.hpp>
#include <engine/gui/Registration.hpp>

namespace engine::gui {

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
		bool TakesInput(const Store &store, Entity instance) {
			if (store.IsA(instance, ButtonClass())) {
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
			// out of its parent still has a rectangle — `Resolved` keeps it
			// deliberately — and clicking where it would have been must not
			// find it.
			if (!command.Bounds.Contains(point) || !command.Clip.Contains(point)) {
				continue;
			}

			if (TakesInput(store, command.Source)) {
				return command.Source;
			}

			// Not `break`. An inactive element is transparent to input, so the
			// walk carries on to whatever is behind it — which is what lets a
			// background panel exist without swallowing the interface it
			// contains.
		}

		return NULL_ENTITY;
	}

	std::span<const GuiEvent>
	Router::Update(const Store &store, const DrawList &list, const Pointer &pointer) {
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
