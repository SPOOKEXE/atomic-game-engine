#pragma once

// arch-waiver public-header: forward ECS API. Component registration and
// inspection use this complete set contract across future consumers.

// An interned, sorted, immutable set of component ids.
//
// Two things are the same object here, and that is the point rather than a
// coincidence:
//
// - **An archetype's identity.** Every entity carrying exactly `{Transform,
//   Motion}` lives in one table, and this is what names it. Interning makes
//   "are these the same archetype" an integer compare instead of a set compare,
//   and makes the archetype lookup a hash of one number.
// - **A class, in the instance model.** `Instance.new("Part")` resolves a class
//   name to the set `{Transform, Bounds, Visual, Collider, Surface}` and lands
//   the new row in the archetype that set names. `:IsA` is then a subset test
//   over sorted arrays.
//
// **Sorted, so the set is canonical.** `{Motion, Transform}` and `{Transform,
// Motion}` are one set with one id, whichever order the caller named them in.
// Sorting is by component id, which is registration order - which is why
// `Components` insists registration happens once, at startup, in a fixed order.
// Two runs that disagree about that would build differently-numbered sets, and
// iteration order would follow.
//
// Sets are never destroyed. A world that creates and destroys archetypes in a
// cycle would otherwise recycle ids, and a stale one would silently name a
// different set. This is the same trade `core::Name` makes.
//
// @tier L3 · shared

#include <engine/ecs/TypeDescriptor.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace engine::ecs {

	// A canonical set of component ids, shared by everything that names it.
	//
	// Obtained from `Intern`, never constructed. A reference handed out stays
	// valid for the life of the process, so an archetype may hold one.
	//
	// @since v0.2
	class ComponentSet {
	  public:
		// The set is interned and lives forever; nobody copies or moves one.
		ComponentSet(const ComponentSet &) = delete;

		// The set is interned and lives forever; nobody assigns one.
		ComponentSet &operator=(const ComponentSet &) = delete;

		// The canonical set for `ids`, sorting and de-duplicating them.
		//
		// Invalid ids are dropped rather than stored, so a caller resolving
		// names out of a corrupt snapshot gets a smaller set instead of one
		// holding a hole.
		//
		// @param ids The component ids to include, in any order.
		// @return The interned set, stable for the life of the process.
		static const ComponentSet &Intern(std::span<const ComponentId> ids);

		// The canonical set for a braced list of ids.
		//
		// @param ids The component ids to include, in any order.
		// @return The interned set.
		static const ComponentSet &Intern(std::initializer_list<ComponentId> ids);

		// The set holding nothing, which is where an entity with no components
		// lives.
		//
		// @return The interned empty set.
		static const ComponentSet &Empty();

		// The number of distinct sets interned so far.
		//
		// @return The current set count, including the empty set.
		static size_t Count();

		// This set's dense id.
		//
		// Comparing two of these is how archetype identity is decided, and
		// ordering by them is what makes archetype iteration deterministic.
		//
		// @return The interned id, counting from zero.
		uint32_t Id() const {
			return Identifier;
		}

		// The component ids, ascending.
		//
		// @return A view valid for the life of the process.
		std::span<const ComponentId> Ids() const {
			return {Members.data(), Members.size()};
		}

		// The number of components in the set.
		//
		// @return The member count.
		size_t Size() const {
			return Members.size();
		}

		// Reports whether the set holds nothing.
		//
		// @return `true` for the empty set.
		bool IsEmpty() const {
			return Members.empty();
		}

		// Reports whether one component is in the set.
		//
		// A binary search, because the members are sorted and a set wide enough
		// for a linear scan to lose is a class with a lot of properties.
		//
		// @param id The component to look for.
		// @return `true` when the set contains it.
		bool Contains(ComponentId id) const;

		// Reports whether every id in `ids` is in the set.
		//
		// This is what a query match is, and what `:IsA` is: a class is a
		// subset of every class that derives from it.
		//
		// @param ids The components to look for, in any order.
		// @return `true` when the set contains all of them.
		bool ContainsAll(std::span<const ComponentId> ids) const;

		// The set with one more component, interned.
		//
		// Returns this set unchanged when it already contains `id`. This and
		// `Without` are the archetype graph's edges - an entity gaining a
		// component follows one to the table it belongs in.
		//
		// @param id The component to add.
		// @return The interned set.
		const ComponentSet &With(ComponentId id) const;

		// The set with one fewer component, interned.
		//
		// Returns this set unchanged when it does not contain `id`.
		//
		// @param id The component to remove.
		// @return The interned set.
		const ComponentSet &Without(ComponentId id) const;

		// Compares interned identities.
		//
		// @param other The set to compare.
		// @return `true` when both are the same interned set.
		bool operator==(const ComponentSet &other) const {
			return Identifier == other.Identifier;
		}

		// Compares interned identities for inequality.
		//
		// @param other The set to compare.
		// @return `true` when the sets differ.
		bool operator!=(const ComponentSet &other) const {
			return Identifier != other.Identifier;
		}

	  private:
		friend class ComponentSetTable;

		ComponentSet() = default;

		uint32_t Identifier = 0;
		std::span<const ComponentId> Members;
	};
}
