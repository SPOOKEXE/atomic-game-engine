#pragma once

// A stable name, and a cheap handle for it.
//
// The rule this type exists to enforce:
//
//     A name crosses boundaries. A number does not.
//
// Anything that has to survive a save file, a wire format, a manifest or a
// rename of the file it was declared in is identified by its **string**. A
// number derived from declaration order is not stable — reorder two
// add_subdirectory lines, or load two scripts in the other order, and every
// saved reference now points somewhere else, silently.
//
// Inside one process none of that matters and a string is the wrong thing to
// compare, hash and index with. So `Name` interns once and hands back a dense
// counter: construction is a hash lookup, and everything afterwards is a
// 32-bit integer compare.
//
//     Name transform("Transform");   // once, at load
//     if (a == b) { ... }            // integer compare, forever after
//
// Ids are assigned by a counter in first-seen order and are **valid only
// within one process**. Serializing `Id()` undoes the entire point of the type.
// Serialize `Text()`.
//
// Reserve() exists for the rare case where the number *is* part of a format
// and was written down by hand. Prefer not to need it.
//
// @tier L0 · shared

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace engine::core {

	// An interned string compared and hashed by a process-local integer handle.
	//
	// The registry is thread-safe, never removes entries, and keeps Text() views
	// valid for the life of the process. Serialize Text(), never Id().
	//
	// @threadsafe
	class Name {
	  public:
		// Ids count up from zero, so the invalid one has to be a value the
		// counter never produces.
		static constexpr uint32_t INVALID = 0xFFFFFFFFu;

		// Creates an invalid name.
		constexpr Name() = default;

		// Interns. Cheap to repeat — the second call for the same text is a
		// hash lookup — but not free, so do it once and keep the result rather
		// than constructing from a literal inside a loop.
		explicit Name(std::string_view text);

		// Pins `text` to exactly `id`, for a number that is itself part of a format.
		//
		// Repeating the same pair is idempotent. Empty text, INVALID, one text with
		// two ids, or two texts with one id returns an invalid Name.
		static Name Reserve(std::string_view text, uint32_t id);

		// The handle for an id already interned, or an invalid Name. Does not
		// intern anything.
		static Name FromId(uint32_t id);

		// Whether `text` has been interned, without interning it.
		static bool Exists(std::string_view text);

		// The number of strings currently interned, including reserved names.
		static size_t Count();

		// The process-local handle, or INVALID for an invalid Name.
		constexpr uint32_t Id() const {
			return Identifier;
		}

		// Reports whether this object refers to an interned string.
		constexpr bool IsValid() const {
			return Identifier != INVALID;
		}

		// Converts explicitly to whether this object is valid.
		constexpr explicit operator bool() const {
			return IsValid();
		}

		// The string this was interned from. Stable for the life of the
		// process — the storage never moves and entries are never removed.
		// Empty for an invalid Name.
		//
		// This is the serialization and diagnostic path, not a hot one.
		std::string_view Text() const;

		// Compares process-local handles for equality.
		constexpr bool operator==(const Name &other) const {
			return Identifier == other.Identifier;
		}

		// Compares process-local handles for inequality.
		constexpr bool operator!=(const Name &other) const {
			return Identifier != other.Identifier;
		}
		// Ordered by id, which is first-seen order rather than alphabetical.
		// Enough for a std::map or a sorted vector; sort on Text() when a
		// person is going to read the result.
		constexpr bool operator<(const Name &other) const {
			return Identifier < other.Identifier;
		}

	  private:
		constexpr explicit Name(uint32_t id) : Identifier(id) {}

		uint32_t Identifier = INVALID;
	};
}

// Hashes a Name by its process-local integer handle.
template <> struct std::hash<engine::core::Name> {
	// Returns the standard uint32_t hash of Name::Id().
	size_t operator()(const engine::core::Name &name) const noexcept {
		return std::hash<uint32_t>{}(name.Id());
	}
};
