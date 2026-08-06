#pragma once

// A property's value, read at run time, in text and back.
//
// **This is here rather than inside the save file because two things need it.**
// A game file writes `Size` as `4, 1, 2` and a properties panel shows the same
// string in an editable field, and both have to parse it back the same way. Two
// implementations of "what does a Vector3 look like as text" is the drift rule 6
// is about, and the symptom is a value you can type into the editor that the
// file cannot read back.
//
// **The switch is on `PropertyType` and never on a name.** `script/Runtime.hpp`
// states that rule for the marshalling layer and it is the same rule here: a
// conversion selected by spelling is a conversion that silently does the wrong
// thing the first time two properties share a name.
//
// **Floating point round-trips exactly.** `std::to_chars` shortest form, not a
// fixed number of decimal places — a save file that lost the low bits of a
// position would make loading and re-saving a scene move it, and the movement
// would be invisible per part and obvious after a hundred loads.
//
// @tier L10 · shared

#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Rect.hpp>
#include <engine/core/types/UDim.hpp>
#include <engine/core/types/Vector2.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace engine::game {

	// One property's value, whatever type it is.
	//
	// **A struct with every field rather than a variant**, because the whole
	// point is a value crossing between a store, a text field and a document
	// with its type carried alongside — and the alternative costs a visitor at
	// each of the three. At around a hundred bytes it is passed by reference
	// and never stored in bulk; the property surface is a per-interaction path,
	// not a per-row one. `Classes.hpp` makes the same argument from the other
	// side: the conversion cost is paid only by callers who arrived by name.
	//
	// @since v0.7
	struct PropertyValue {
		// Which of the fields below is meaningful.
		ecs::PropertyType Type = ecs::PropertyType::Opaque;

		// A `PropertyType::Bool` value.
		bool Bool = false;

		// A `PropertyType::Int32` value.
		int32_t Int32 = 0;

		// A `PropertyType::Int64` value.
		int64_t Int64 = 0;

		// A `PropertyType::Float` value.
		float Float = 0.0f;

		// A `PropertyType::Double` value.
		double Double = 0.0;

		// Used for both `Name` and `Enum`. The storage is identical and the
		// contract is not — see `PropertyType::Enum`, which is checked against
		// `ecs::EnumTable` on the way in.
		core::Name Name;

		// A handle within one world, meaningless outside it. A document turns
		// this into its own local id; see `Game.hpp`.
		ecs::Entity Reference;

		// A `PropertyType::Vector3` value.
		core::Vector3 Vector3;

		// A `PropertyType::CFrame` value.
		core::CFrame CFrame;

		// A `PropertyType::Color3` value.
		core::Color3 Color3;

		// A `PropertyType::Vector2` value.
		core::Vector2 Vector2;

		// A `PropertyType::UDim` value.
		core::UDim UDim;

		// A `PropertyType::UDim2` value.
		core::UDim2 UDim2;

		// A `PropertyType::Rect` value.
		core::Rect Rect;
	};

	// Reads one property off an instance.
	//
	// @param store      The world.
	// @param instance   The instance.
	// @param descriptor The property to read.
	// @param out        Filled in on success, with `Type` set from `descriptor`.
	// @return `false` when the instance does not carry what the getter reads.
	bool ReadProperty(
		const ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &descriptor,
		PropertyValue &out
	);

	// Writes one property onto an instance.
	//
	// @param store      The world.
	// @param instance   The instance.
	// @param descriptor The property to write.
	// @param value      The value, whose `Type` must match the descriptor's.
	// @return `false` on a type mismatch, a read-only property, an adopt-only
	//         store, or an instance that cannot take the write.
	bool WriteProperty(
		ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &descriptor,
		const PropertyValue &value
	);

	// A number as the shortest text that reads back as the same value.
	//
	// **Exposed because a document has numbers that are not property values** —
	// a world's tick rate, a version — and `std::to_string` is `%f`: six decimal
	// places, so it writes 60 as "60.000000" and 1e-8 as "0.000000". One
	// formatting rule for everything a file holds is the point; two would drift
	// the first time somebody rounded a tick rate.
	//
	// @param value The number.
	// @return The shortest round-tripping text.
	std::string FormatNumber(double value);

	// The text form of a value.
	//
	// **A `Reference` formats as empty**, because a handle has no text form
	// that means anything outside the world holding it. A document that carries
	// references resolves them itself — see `Game.hpp` — and a properties panel
	// shows the target's name, which it has to look up anyway.
	//
	// @param value The value.
	// @return The text.
	std::string FormatValue(const PropertyValue &value);

	// Parses the text form back.
	//
	// @param type   Which type to read it as.
	// @param text   The text, as `FormatValue` produced it.
	// @param out    Filled in on success.
	// @param reason Filled in with what was expected, on failure. Written for
	//               somebody typing into a field, because that is where this
	//               fails most: "expected x, y, z" is a usable message and
	//               "parse error" is not.
	// @return `false` when the text is not a value of that type.
	bool ParseValue(ecs::PropertyType type, std::string_view text, PropertyValue &out, std::string &reason);

	// Whether two values of the same type are identical.
	//
	// **Bitwise on the floats, not within a tolerance.** This decides whether a
	// property is written to a save file, and "close enough to the default"
	// would drop a deliberate nudge and then reload it as the default.
	//
	// @param left  One value.
	// @param right The other.
	// @return `true` when they are the same type and the same value.
	bool ValuesEqual(const PropertyValue &left, const PropertyValue &right);

	// The text a `PropertyType` is written under in a document.
	//
	// Not `ecs::Describe(PropertyType)`: that one is for a human reading a log
	// and may be reworded, and this one is in a file format. Two callers, two
	// stabilities — the same reason a wire format and a log message are never
	// the same string.
	//
	// @param type The type.
	// @return A view valid for the lifetime of the process.
	std::string_view TypeTag(ecs::PropertyType type);

	// The `PropertyType` a document's tag names.
	//
	// @param tag  The tag, as `TypeTag` wrote it.
	// @param out  Filled in on success.
	// @return `false` when this build has no such type.
	bool TypeFromTag(std::string_view tag, ecs::PropertyType &out);
}
