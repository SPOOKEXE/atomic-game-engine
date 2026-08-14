#include "Utf8.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/gui/Services.hpp>

#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace engine::gui {

	namespace {
		using core::Rect;
		using core::Vector2;
		using ecs::Entity;
		using ecs::Store;

		// What the service instance is called, and therefore what
		// `game:GetService("GuiService")` finds.
		//
		// A root in the tree rather than a resource, because that is how
		// `GetService` resolves anything: it looks for a root of that name. A
		// service reachable some other way would be a second mechanism for one
		// job.
		constexpr std::string_view GUI_SERVICE = "GuiService";

		// Whether an element can hold the selection.
		//
		// `Selectable` and nothing else — deliberately not "or it is a
		// `GuiButton`", which is the rule for *input*. A button is clickable by
		// default because a pointer is aimed; selection is moved a step at a
		// time and a game that did not opt an element in did not want the
		// selection to stop there.
		bool Selectable(const Store &store, Entity instance) {
			const Element *element = store.Get<Element>(instance);
			return element != nullptr && element->Visible && element->Selectable;
		}

		// The middle of an element's drawn rectangle, or nothing.
		//
		// Taken from the compiled list rather than from `Resolved`, because the
		// list is what says an element is on screen at all — `Resolved` keeps
		// its rectangle after the element stops being drawn, which is
		// deliberate and is exactly the wrong source for "where can the
		// selection go".
		bool CentreOf(const DrawList &list, Entity instance, Vector2 &out) {
			for (const DrawCommand &command : list.Commands) {
				if (command.Source == instance) {
					out = Vector2{
						(command.Bounds.Min.X + command.Bounds.Max.X) * 0.5f,
						(command.Bounds.Min.Y + command.Bounds.Max.Y) * 0.5f,
					};
					return true;
				}
			}
			return false;
		}

		// Whether `candidate` lies in `move`'s direction from `from`, and how
		// good a step it is.
		//
		// **Along the axis first, across it as the tiebreak.** A player pressing
		// up means "the nearest thing above", and of two things equally above,
		// the one more nearly straight up. Scoring rather than sorting, because
		// "is above" is not a total order — B can be above A while A is above C
		// — and a comparator over a non-order is where a sort produces a
		// different answer depending on the input's initial arrangement.
		//
		// The across-axis term is weighted so it only ever breaks ties: a
		// candidate further along the axis never wins against a nearer one by
		// being better aligned, which is the failure that makes a selection jump
		// across the screen.
		bool Scored(SelectionMove move, const Vector2 &from, const Vector2 &to, float &score) {
			const float dx = to.X - from.X;
			const float dy = to.Y - from.Y;

			float along = 0.0f;
			float across = 0.0f;

			switch (move) {
			case SelectionMove::Up:
				along = -dy;
				across = std::abs(dx);
				break;
			case SelectionMove::Down:
				along = dy;
				across = std::abs(dx);
				break;
			case SelectionMove::Left:
				along = -dx;
				across = std::abs(dy);
				break;
			case SelectionMove::Right:
				along = dx;
				across = std::abs(dy);
				break;
			}

			// **Strictly forward**, so an element whose centre is level with
			// this one is not "above" it. Without this a row of buttons selects
			// itself when a player presses up.
			if (along <= 0.0f) {
				return false;
			}

			score = along + across * 0.125f;
			return true;
		}
	}

	Entity InstallGuiServices(Store &store) {
		RegisterGuiClasses();

		Entity service = GuiServiceOf(store);

		if (service == ecs::NULL_ENTITY) {
			// **A replica does not author its own services, so it waits for
			// one.** An instance minted here would take an authoritative index
			// the authority is also handing out, which is what
			// `Store::SetAdoptOnly` refuses — and even if it did not, the join
			// snapshot applies `ApplyMode::Authoritative` and sweeps everything
			// it does not mention. The service arrives with the world instead.
			if (store.AdoptOnly()) {
				return ecs::NULL_ENTITY;
			}

			// A root, which is where `GetService` looks. Left unparented rather
			// than put under anything: `scene::InstallServices` does the same,
			// and a service under another service is a tree a script would have
			// to know the shape of.
			service = store.CreateInstance(GuiClass(GUI_SERVICE), std::string(GUI_SERVICE));
			if (service == ecs::NULL_ENTITY) {
				return ecs::NULL_ENTITY;
			}
		}

		// **The row can arrive without the state, and the state is what the
		// service is for.** What crosses a wire is what an instance *is* —
		// `ecs.Hierarchy`, `ecs.InstanceName`, `ecs.InstanceClass` — and no
		// `gui.` component is replicated, so a client is shown a `GuiService`
		// carrying nothing. `Select` and `Focus` both read this component and
		// both answer `false` without it, which is a client whose keyboard never
		// reaches a `TextBox` and no error saying why.
		if (store.Get<GuiServiceState>(service) == nullptr) {
			store.Set(service, GuiServiceState{});
		}

		return service;
	}

	Entity GuiServiceOf(const Store &store) {
		return store.FindFirstRoot(GUI_SERVICE);
	}

	Vector2 GuiInset(const Screen &screen) {
		return Vector2{0.0f, screen.TopInset};
	}

	bool Select(Store &store, Entity instance) {
		const Entity service = GuiServiceOf(store);
		if (service == ecs::NULL_ENTITY) {
			return false;
		}

		GuiServiceState *state = store.GetMutable<GuiServiceState>(service);
		if (state == nullptr) {
			return false;
		}

		// **Refused rather than accepted**, for the reason `Services.hpp`
		// gives: a selection parked on something no move can leave is a state a
		// player recovers from by restarting.
		if (instance != ecs::NULL_ENTITY && !Selectable(store, instance)) {
			return false;
		}

		if (state->SelectedObject == instance) {
			return false;
		}

		state->SelectedObject = instance;
		return true;
	}

	bool SelectNext(Store &store, const DrawList &list, SelectionMove move) {
		const Entity service = GuiServiceOf(store);
		if (service == ecs::NULL_ENTITY) {
			return false;
		}

		const GuiServiceState *state = store.Get<GuiServiceState>(service);
		if (state == nullptr) {
			return false;
		}

		const Entity current = state->SelectedObject;

		// **Each element once**, because one emits up to four commands — a
		// background, an outline, an image and a label — and scoring it four
		// times would make an element with a border beat one without for no
		// reason a player could see.
		std::unordered_set<uint64_t> seen;

		if (current == ecs::NULL_ENTITY || !Selectable(store, current)) {
			// Nothing selected, or what was selected has gone. Seed from the
			// first selectable element in paint order.
			if (!state->AutoSelectGuiEnabled) {
				return false;
			}

			for (const DrawCommand &command : list.Commands) {
				if (!seen.insert(command.Source.Id).second) {
					continue;
				}
				if (Selectable(store, command.Source)) {
					return Select(store, command.Source);
				}
			}
			return false;
		}

		Vector2 from;
		if (!CentreOf(list, current, from)) {
			// Selected but not drawn this frame — scrolled away, or its
			// collector was disabled. Nothing to move relative to.
			return false;
		}

		Entity best = ecs::NULL_ENTITY;
		float bestScore = std::numeric_limits<float>::max();

		for (const DrawCommand &command : list.Commands) {
			const Entity candidate = command.Source;
			if (candidate == current || !seen.insert(candidate.Id).second) {
				continue;
			}
			if (!Selectable(store, candidate)) {
				continue;
			}

			Vector2 to;
			if (!CentreOf(list, candidate, to)) {
				continue;
			}

			float score = 0.0f;
			if (!Scored(move, from, to, score)) {
				continue;
			}

			if (score < bestScore) {
				bestScore = score;
				best = candidate;
			}
		}

		if (best == ecs::NULL_ENTITY) {
			return false;
		}
		return Select(store, best);
	}

	Entity FocusedTextBox(const Store &store) {
		const Entity service = GuiServiceOf(store);
		if (service == ecs::NULL_ENTITY) {
			return ecs::NULL_ENTITY;
		}

		const GuiServiceState *state = store.Get<GuiServiceState>(service);
		if (state == nullptr) {
			return ecs::NULL_ENTITY;
		}

		// **Both tests, and the second is not redundant.** `Alive` catches the
		// box being destroyed; `Entry` catches the row being alive and no longer
		// a text box, which is what a store that recycled the index into
		// something else looks like from here. Either way the answer is "nobody
		// is typing", which is the only answer a caller can act on.
		if (!store.Alive(state->FocusedTextBox) || store.Get<Entry>(state->FocusedTextBox) == nullptr) {
			return ecs::NULL_ENTITY;
		}
		return state->FocusedTextBox;
	}

	bool Focus(Store &store, Entity textBox) {
		const Entity service = GuiServiceOf(store);
		if (service == ecs::NULL_ENTITY) {
			return false;
		}

		GuiServiceState *state = store.GetMutable<GuiServiceState>(service);
		if (state == nullptr) {
			return false;
		}

		// **`Entry` is the `TextBox` test**, the way `Element` is the
		// `GuiObject` test one file along: the component is on that class and on
		// no other, so this needs no class lookup and cannot drift from the class
		// tree the way a name comparison would.
		if (textBox != ecs::NULL_ENTITY && store.Get<Entry>(textBox) == nullptr) {
			return false;
		}

		const Entity previous = FocusedTextBox(store);
		if (previous == textBox) {
			return false;
		}

		// **The old box's caret is put back before the new one takes it**, so a
		// world in which two boxes both read as focused never exists, not even
		// between two statements.
		if (Entry *releasing = previous != ecs::NULL_ENTITY ? store.GetMutable<Entry>(previous) : nullptr;
			releasing != nullptr) {
			releasing->CursorPosition = -1;
			releasing->SelectionStart = -1;
		}

		state->FocusedTextBox = textBox;

		Entry *taking = textBox != ecs::NULL_ENTITY ? store.GetMutable<Entry>(textBox) : nullptr;
		if (taking == nullptr) {
			return true;
		}

		// **`ClearTextOnFocus` empties the box at the moment focus is taken**,
		// which is Roblox's behaviour and the reason the property is worth
		// having: a search field that keeps last search's text is one every
		// player has to clear by hand.
		if (taking->ClearTextOnFocus) {
			if (Label *label = store.GetMutable<Label>(textBox); label != nullptr) {
				label->Text.clear();
			}
		}

		// **After the text is decided, because the caret is counted off it.**
		// One-based and in characters — see `Entry::CursorPosition`.
		const Label *label = store.Get<Label>(textBox);
		taking->CursorPosition = static_cast<int32_t>(Characters(label != nullptr ? label->Text : "")) + 1;
		taking->SelectionStart = -1;
		return true;
	}

	size_t ResetPlayerGui(Store &store, Entity player) {
		// The player's own container, which `scene::AddPlayer` makes beside
		// every player. Absent means a host built this player some other way;
		// there is nowhere to put anything, so nothing is done.
		const Entity target = store.FindFirstChild(player, PLAYER_GUI);
		if (target == ecs::NULL_ENTITY) {
			return 0;
		}

		// **Step 1 and 2: clear out what this life is not keeping.** Collected
		// first and destroyed after, because destroying inside a walk of the
		// children is destroying what the walk is holding.
		//
		// **`ResetOnSpawn` is read off `Layer`, and anything with no `Layer` is
		// left alone.** A collector has one; a `Folder` a script put there does
		// not, and Roblox does not clear those either — what the field describes
		// is a *collector's* lifetime.
		std::vector<Entity> clearing;
		std::unordered_set<core::Name> surviving;

		store.EachChild(target, [&](Entity existing) {
			const Layer *layer = store.Get<Layer>(existing);
			if (layer != nullptr && layer->ResetOnSpawn) {
				clearing.push_back(existing);
				return;
			}
			surviving.insert(store.InstanceNameOf(existing));
		});

		for (const Entity going : clearing) {
			store.DestroyInstance(going);
		}

		// **Step 3: the template, cloned in.** `StarterGui` is a root like every
		// other service — see `STARTER_GUI` for why this module spells the name
		// again rather than linking `scene` for it.
		const Entity starter = store.FindFirstRoot(STARTER_GUI);
		if (starter == ecs::NULL_ENTITY) {
			return 0;
		}

		// **Collected before cloning, for `clearing`'s reason one block up**:
		// each clone is parented into the player and the template is a different
		// subtree, but a walk that mutates the store while the store is walking
		// itself is the shape that bites eventually rather than immediately.
		std::vector<Entity> sources;
		store.EachChild(starter, [&](Entity source) {
			// **A survivor of the same name is the copy the player already
			// has.** Cloning beside it would leave them holding two, one of
			// which nothing updates — and the one a script has a handle to would
			// be whichever it found first.
			if (surviving.count(store.InstanceNameOf(source)) == 0) {
				sources.push_back(source);
			}
		});

		size_t cloned = 0;
		for (const Entity source : sources) {
			const Entity copy = store.CloneInstance(source);
			if (copy == ecs::NULL_ENTITY) {
				continue;
			}

			store.SetParent(copy, target);
			cloned++;
		}

		return cloned;
	}
}
