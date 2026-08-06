#include <engine/ecs/Store.hpp>
#include <engine/gui/Adornments.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>

#include <algorithm>
#include <vector>

namespace engine::gui {

	namespace {
		using ecs::Entity;
		using ecs::Store;

		// The containers an adornment may live in, by name.
		//
		// **The same three `Layout` uses and for the same reason** — they are
		// `scene`'s services and this module may not link `scene`, so the
		// strings are duplicated and pinned by a test at each end. The
		// constants come from `Layout.hpp` rather than being spelled a third
		// time here, which is the difference between one duplication and two.
		struct Containers {
			core::Name Workspace{WORKSPACE};
			core::Name StarterGui{STARTER_GUI};
			core::Name PlayerGui{PLAYER_GUI};
		};

		const Containers &Roots() {
			static const Containers names;
			return names;
		}
	}

	Entity AdorneeOf(const Store &store, Entity adornment) {
		const Adornment *state = store.Get<Adornment>(adornment);
		if (state == nullptr) {
			return ecs::NULL_ENTITY;
		}

		// **Set wins, and an unset one means the parent.** Not "the parent when
		// the adornee is dead", which would be a silent fallback: an `Adornee`
		// pointing at something destroyed is an adornment about nothing, and
		// quietly re-aiming it at whatever it happens to be parented to would
		// draw a box around the wrong object.
		if (state->Adornee != ecs::NULL_ENTITY) {
			return store.Alive(state->Adornee) ? state->Adornee : ecs::NULL_ENTITY;
		}

		return store.ParentOf(adornment);
	}

	bool AdornmentDrawn(const Store &store, Entity adornment) {
		const Adornment *state = store.Get<Adornment>(adornment);
		if (state == nullptr || !state->Visible) {
			return false;
		}

		if (AdorneeOf(store, adornment) == ecs::NULL_ENTITY) {
			return false;
		}

		const Containers &roots = Roots();

		for (Entity above = store.ParentOf(adornment); above != ecs::NULL_ENTITY;
			 above = store.ParentOf(above)) {
			const core::Name name = store.InstanceNameOf(above);
			if (name == roots.Workspace || name == roots.StarterGui || name == roots.PlayerGui) {
				return true;
			}
		}

		return false;
	}

	void EachAdornment(Store &store, const std::function<void(Entity adornment, Entity adornee)> &body) {
		// **Collected before anything is called**, exactly as `Layout` collects
		// its collectors: a body may write a component, which can move a row
		// between archetypes the first time it gets one — and moving a row out
		// from under the query walking it is what `Store::Each`'s deferral
		// exists to prevent.
		struct Drawn {
			Entity Adornment;
			Entity Adornee;
			int32_t ZIndex;
		};

		std::vector<Drawn> drawn;
		store.Each<const Adornment>([&](Entity entity, const Adornment &state) {
			if (!AdornmentDrawn(store, entity)) {
				return;
			}
			drawn.push_back(Drawn{entity, AdorneeOf(store, entity), state.ZIndex});
		});

		// Stable, so two adornments sharing a `ZIndex` keep the order the store
		// held them in rather than swapping between frames.
		std::stable_sort(drawn.begin(), drawn.end(), [](const Drawn &left, const Drawn &right) {
			return left.ZIndex < right.ZIndex;
		});

		for (const Drawn &entry : drawn) {
			body(entry.Adornment, entry.Adornee);
		}
	}
}
