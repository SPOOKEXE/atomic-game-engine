#pragma once

// An entity handle.
//
// A bare integer, deliberately: nothing crossing a world boundary is a pointer,
// and an entity reference is the case that would tempt somebody most. A handle
// that carries a store pointer works right up until the day worlds are
// processes, at which point every one of them is a use-after-free.
//
// @tier L3 · shared

#include <cstdint>
#include <functional>

namespace engine::ecs {

	// A value handle identifying one generation of an entity in a Store.
	//
	// The handle carries no Store pointer. Use Store::Alive to ask whether it is
	// still live in a particular store.
	struct Entity {
		// The layout is the backing store's: a generation in the high bits and
		// an index in the low ones. Neither is exposed, because reading them
		// from outside the store is how code starts depending on the layout.
		uint64_t Id = 0;

		// Constructs the null entity handle.
		constexpr Entity() = default;

		// Constructs a handle from the backing store's complete entity id.
		//
		// @param id The generation-bearing entity id.
		constexpr explicit Entity(uint64_t id) : Id(id) {}

		// Reports whether two handles identify the same entity generation.
		//
		// @param other The handle to compare.
		// @return `true` when both complete ids are equal.
		constexpr bool operator==(const Entity &other) const {
			return Id == other.Id;
		}

		// Reports whether two handles identify different entity generations.
		//
		// @param other The handle to compare.
		// @return `true` when the complete ids differ.
		constexpr bool operator!=(const Entity &other) const {
			return Id != other.Id;
		}

		// Tests only whether this is the null handle. A destroyed or stale handle
		// remains non-null; use Store::Alive() to ask whether it is still live in a
		// particular store.
		//
		// @return `false` only for the null handle.
		constexpr explicit operator bool() const {
			return Id != 0;
		}
	};

	// The entity handle that belongs to no Store.
	inline constexpr Entity NULL_ENTITY{};
}

// Hashes Entity by its complete generation-bearing id.
template <> struct std::hash<engine::ecs::Entity> {
	// Computes a hash suitable for standard unordered containers.
	//
	// @param entity The entity handle to hash.
	// @return The hash of `entity.Id`.
	size_t operator()(const engine::ecs::Entity &entity) const noexcept {
		return std::hash<uint64_t>{}(entity.Id);
	}
};
