#pragma once

// Names an instance class by its dense id, for headers that only store or pass
// one.
//
// `Classes.hpp` brings in the class registry, component sets and the component
// table. A header that only returns or keeps a `ClassId` includes this instead,
// so editing the registry does not rebuild it.
//
// @tier L3 · shared

#include <cstdint>

namespace engine::ecs {

	// A dense process-local handle for one registered class.
	//
	// @since v0.2
	struct ClassId {
		// The value no registration produces.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// The registration index, or INVALID.
		uint32_t Index = INVALID;

		// Creates an invalid id.
		constexpr ClassId() = default;

		// Creates an id from a registration index.
		//
		// @param index The registration index to wrap.
		constexpr explicit ClassId(uint32_t index) : Index(index) {}

		// Reports whether this id names a registered class.
		//
		// @return `true` when the id came from a registration.
		constexpr bool IsValid() const {
			return Index != INVALID;
		}

		// Compares registration indices for equality.
		//
		// @param other The id to compare.
		// @return `true` when both name the same class.
		constexpr bool operator==(const ClassId &other) const {
			return Index == other.Index;
		}

		// Compares registration indices for inequality.
		//
		// @param other The id to compare.
		// @return `true` when the ids name different classes.
		constexpr bool operator!=(const ClassId &other) const {
			return Index != other.Index;
		}
	};
}
