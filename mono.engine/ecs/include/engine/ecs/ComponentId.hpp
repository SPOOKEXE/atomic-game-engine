#pragma once

// Names a component type by its dense id, for headers that only store or pass
// one.
//
// `TypeDescriptor.hpp` and `Components.hpp` bring in byte streams, names and
// the registration templates. A header that only keeps a `ComponentId` in a
// field or a signature includes this instead, so editing the registry does not
// rebuild it.
//
// @tier L3 · shared

#include <cstdint>

namespace engine::ecs {

	// A dense process-local handle for one registered component type.
	//
	// Assigned in registration order, so two runs of the same binary that
	// register the same types in the same order agree - which is what lets an
	// archetype be identified by a sorted list of these rather than by names.
	// Registration order is fixed by doing it at startup; see Components.
	//
	// @since v0.2
	struct ComponentId {
		// The value no registration produces.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The registration index, or INVALID.
		uint32_t Index = INVALID;

		// Creates an invalid id.
		constexpr ComponentId() = default;

		// Creates an id from a registration index.
		//
		// @param index The registration index to wrap.
		constexpr explicit ComponentId(uint32_t index) : Index(index) {}

		// Reports whether this id names a registered type.
		//
		// @return `true` when the id came from a registration.
		constexpr bool IsValid() const {
			return Index != INVALID;
		}

		// Compares registration indices for equality.
		//
		// @param other The id to compare.
		// @return `true` when both name the same type.
		constexpr bool operator==(const ComponentId &other) const {
			return Index == other.Index;
		}

		// Compares registration indices for inequality.
		//
		// @param other The id to compare.
		// @return `true` when the ids name different types.
		constexpr bool operator!=(const ComponentId &other) const {
			return Index != other.Index;
		}

		// Orders by registration index, which is what keeps a component set
		// canonical: one sorted list per set of types, whatever order the
		// caller named them in.
		//
		// @param other The id to compare.
		// @return `true` when this id was registered first.
		constexpr bool operator<(const ComponentId &other) const {
			return Index < other.Index;
		}
	};
}
