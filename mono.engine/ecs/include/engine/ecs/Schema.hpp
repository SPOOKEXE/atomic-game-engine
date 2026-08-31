#pragma once

// A component type nobody wrote a C++ struct for.
//
// Every other component in the engine is a `T` the compiler knows about, and
// `Components::Register<T>` derives the whole `TypeDescriptor` from it. That
// covers the engine and it cannot cover a *game*: a script declaring `Health`
// with a `Current` and a `Max` has no `T`, and there is no build step between
// writing that line and the world having to store it.
//
// So a schema is the same descriptor built the other way round - from a list of
// named fields and their `PropertyType`s - and everything downstream is
// unchanged. The id is a `ComponentId` from the same counter, the storage is a
// `Column` like any other, `Store::SetComponent` writes it, a query matches it,
// and a snapshot carries it. **Nothing in the storage learns that a component
// was described rather than declared.**
//
// ## Three decisions worth reading before editing this
//
// **The layout is derived, never the caller's order.** Fields are sorted by
// alignment descending and then by name, so two processes handed the same field
// set lay it out identically whatever order they named them in. That is not
// tidiness: the caller is usually a script, a Luau table iterates in hash order,
// and a layout that followed the caller would differ between two runs of one
// file - which is a snapshot from one process that another cannot read.
//
// **Padding is zeroed rather than left alone.** `TypeDescriptor`'s own warning
// says a snapshotted component must have none, because uninitialised padding
// makes two runs of one scene produce different bytes. A derived layout cannot
// promise there is no padding, so it promises the padding is *defined*: every
// blob is zeroed before its fields are constructed, and every copy rebuilds the
// destination the same way.
//
// **Serialisation is field by field and never the object representation.** A
// `Name` field holds a process-local id, and `Name.hpp` says never to serialize
// one - so the raw path `DescribeType` would have installed is exactly the bug
// that header warns about. Going through the fields costs a loop in a path that
// runs at save time, and it is the only form that is correct for every field
// type at once.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {

	// How a described field occupies its component row.
	//
	// The property type remains the value scripts and Studio see. Packing only
	// changes the resident representation. This keeps quantisation out of the
	// save and replication formats, which have separate contracts.
	//
	// `Float16` is IEEE binary16. `Float8` is signed normalised over `[-1, 1]`,
	// while `UFloat8` and `UFloat16` are unsigned normalised over `[0, 1]`.
	// Integer formats saturate at their signed or unsigned range. Four-bit
	// integers and bools share bytes with adjacent sub-byte fields.
	//
	// @since v0.20
	enum class FieldPacking : uint8_t {
		Native,
		Float16,
		UFloat16,
		Float8,
		UFloat8,
		Int16,
		UInt16,
		Int8,
		UInt8,
		Int4,
		UInt4,
		Bool,
	};

	// The stable spelling used by scripts and Studio.
	//
	// @since v0.20
	const char *Describe(FieldPacking packing);

	// One field, as a caller declares it.
	//
	// @since v0.12
	struct FieldSpec {
		// The field's name, as a script spells it.
		std::string_view Name;

		// What the field holds. `PropertyType::Opaque` is refused - a field
		// nothing can read or write is a column of bytes with no author.
		PropertyType Type = PropertyType::Opaque;

		// Which registered set an `Enum` field's value must belong to.
		//
		// Ignored for every other type, exactly as `PropertyDescriptor::EnumName`
		// is. Named rather than pointed at, so the enum may be registered after
		// the schema that uses it.
		std::string_view Enum = {};

		// The resident representation. A bool is always normalised to `Bool`,
		// because spending a byte on a runtime-derived bit defeats the layout's
		// ability to share that byte with adjacent flags.
		FieldPacking Packing = FieldPacking::Native;
	};

	// One field, as the storage holds it.
	//
	// @since v0.12
	struct FieldDescriptor {
		// The field's name.
		core::Name Name;

		// That same name's text, resolved once. `PropertyDescriptor::Spelling`
		// exists for this reason and this is the same reason: a binding matching
		// a script's key against a field list would otherwise take the
		// process-wide name registry's lock once per field per access.
		std::string_view Spelling;

		// What the field holds.
		PropertyType Type = PropertyType::Opaque;

		// Descriptive metadata tags attached to this field.
		std::vector<std::string> Tags;

		// Whether Studio should surface this field as an exposed config value.
		bool Exposed = false;

		// The set an `Enum` field's value must belong to. Invalid otherwise.
		core::Name Enum;

		// Where the value starts within the component's blob.
		uint32_t Offset = 0;

		// How many bytes it occupies.
		uint32_t Size = 0;

		// The resident representation and its exact location within `Offset`.
		// Native and byte-sized formats start at bit zero. Sub-byte fields may
		// share the same byte.
		FieldPacking Packing = FieldPacking::Native;
		// Bit position within the field byte and total resident storage width.
		//@{
		uint8_t BitOffset = 0;
		uint32_t StorageBits = 0;
		//@}
	};

	// A component type described at run time.
	//
	// Immutable once registered. A schema is reachable from its `ComponentId`
	// for as long as the process lives, which is what lets a binding read a
	// value back out of a column it only knows the id of.
	//
	// @since v0.12
	class Schema {
	  public:
		// The name the component is registered under.
		//
		// @return The stable identity, which is what a file carries.
		core::Name Name() const {
			return TypeName;
		}

		// Every field, in layout order.
		//
		// **Layout order, which is neither declaration order nor alphabetical.**
		// A caller listing fields for a person to read should sort by
		// `Spelling`; this order exists so that two builds agree about offsets.
		//
		// @return The fields.
		std::span<const FieldDescriptor> Fields() const {
			return Layout;
		}

		// The field with a name.
		//
		// @param field The field's name.
		// @return The descriptor, or `nullptr` when the schema has no such field.
		const FieldDescriptor *Find(core::Name field) const;

		// The field with a name, compared as text.
		//
		// **The form a binding should call.** Interning a key taken off a script
		// takes the name registry's lock and adds a hash, which is the
		// measurement `PropertyDescriptor::Spelling` records - and a field access
		// is at least as hot as a property access.
		//
		// @param field The field's name, as the caller spells it.
		// @return The descriptor, or `nullptr` when the schema has no such field.
		const FieldDescriptor *Find(std::string_view field) const;

		// Metadata tags attached to this component by its author.
		std::vector<std::string> Tags() const;

		// How many bytes one value of this component occupies.
		//
		// @return The blob size, including any padding the layout needed.
		uint32_t Size() const {
			return Width;
		}

		// The alignment one value needs.
		//
		// @return The widest field's alignment, or 1 for a schema with no fields.
		uint32_t Alignment() const {
			return Align;
		}

	  private:
		friend class Schemas;

		core::Name TypeName;
		std::vector<FieldDescriptor> Layout;
		std::vector<std::string> TagNames;
		uint32_t Width = 0;
		uint32_t Align = 1;

		// This schema's `ComponentId` slot, whose *address* is what tells the
		// component table that two registrations under one name are the same
		// type rather than two types colliding. See `Components::Adopt`.
		ComponentId Slot;
	};

	// Registers schemas and answers questions about them.
	//
	// Process-wide, like `Components`, `Classes` and `EnumTable`, and for the
	// reason all four give: a component has to mean the same thing in every
	// world, because a snapshot taken in one is restored into another.
	//
	// @since v0.12
	// @threadsafe
	class Schemas {
	  public:
		// Why a registration was refused.
		//
		// Returned rather than aborted, which is the one place this diverges
		// from `Components::Of<T>()` - and the divergence is the caller.
		// `Of<T>()` aborts because a C++ type that reached a sealed table has
		// nobody to tell; a schema's caller is a script, an editor or a game
		// file, and every one of those can be handed an error and carry on.
		//
		// @since v0.12
		enum class Status : uint8_t {
			// Registered, or already registered with exactly this field set.
			Ok,

			// A field named a type the storage cannot hold - `Opaque`, or a
			// spelling nothing maps.
			BadField,

			// Two fields share a name.
			DuplicateField,

			// The name is already registered, and by something with a different
			// field set - or by a C++ type, which is the same collision.
			Conflict,

			// The component table is closed. Registering now would take an id
			// decided by whichever world happened to run first.
			Sealed,

			// The name is empty.
			Unnamed,

			// This process has described as many components as it can. See the
			// cap in `Schema.cpp` and the reason it exists.
			Exhausted,
		};

		// What a registration produced.
		//
		// @since v0.12
		struct Result {
			// The component's id, or an invalid one when `Why` is not `Ok`.
			ComponentId Id;

			// Whether it worked, and what stopped it when it did not.
			Status Why = Status::Ok;

			// Whether this registration created the component rather than
			// agreeing with one already there.
			//
			// Not an error either way - two scripts may both declare that
			// `Health` has a `Current` and a `Max`, and the second is agreeing.
			// Reported because a caller building an editor list wants to know.
			bool Created = false;
		};

		// Registers a component described by its fields, or agrees with one
		// already registered under that name.
		//
		// **Idempotent for an identical field set**, which is `EnumTable`'s rule
		// and is what makes two scripts declaring the same component legal. An
		// identical set means the same names carrying the same types and the
		// same enums; field *order* is not part of it, because the layout is
		// derived rather than taken from the caller.
		//
		// @param name   The stable name, as a file and a script spell it.
		// @param fields The fields, in any order.
		// @return The id and what happened.
		static Result Register(std::string_view name, std::span<const FieldSpec> fields);

		// The schema behind a component id.
		//
		// @param component The id to describe.
		// @return The schema, or `nullptr` when that component is a C++ type
		//         rather than a described one.
		static const Schema *Of(ComponentId component);

		// The schema registered under a name.
		//
		// @param name The registered name.
		// @return The schema, or `nullptr` when nothing described is registered
		//         under it.
		static const Schema *Find(core::Name name);

		// Replaces the metadata tags on a component. Tags are descriptive names
		// such as `deprecated`, `experiment`, or `constant`.
		static bool SetTags(ComponentId component, std::span<const std::string_view> tags);

		// Replaces the metadata tags on one field of a described component.
		static bool
		SetFieldTags(ComponentId component, core::Name field, std::span<const std::string_view> tags);
		// Changes whether Studio exposes one described field.
		static bool SetFieldExposed(ComponentId component, core::Name field, bool exposed);

		// Returns a copy so callers do not retain a view across registry access.
		static std::vector<std::string> Tags(ComponentId component);
		// Returns a copy of one field's descriptive metadata tags.
		static std::vector<std::string> FieldTags(ComponentId component, core::Name field);

		// Every described component, in registration order.
		//
		// **A copy of the ids rather than a view of the table**, because the
		// table is behind a mutex and registration continues while a caller
		// walks. The count is the number of components a game declared, which is
		// tens rather than thousands.
		//
		// @return The ids, oldest first.
		static std::vector<ComponentId> All();

		// Forgets every described component.
		//
		// **For tests, and for a host tearing one universe down before building
		// another.** It does not unregister anything from `Components` - an id,
		// once minted, means that type for the life of the process, which is the
		// property the whole table rests on. What it releases is the *schemas*,
		// so a second registration under a name this one used is a fresh
		// registration rather than a conflict with a definition nothing holds.
		//
		// A running world must not call this: its columns reach their schema to
		// destroy their rows.
		static void Clear();

		// The `PropertyType` a spelling names.
		//
		// **One vocabulary for both script runtimes, the manifest and any game
		// file**, rather than a table per caller that drifts. The engine's own
		// spellings - the ones `Describe(PropertyType)` hands back - are
		// accepted, and so are the three a script author would reach for first:
		// `number`, `boolean` and `string`.
		//
		// `number` is a `double`, which is what a Luau number is and what a
		// JavaScript number is. An author who wants four bytes says `float`.
		//
		// @param spelling The type name.
		// @param out      Filled in on a match.
		// @return `false` when nothing maps that spelling.
		static bool TypeNamed(std::string_view spelling, PropertyType &out);

		// Resolves a script field spelling to its logical type and resident
		// representation. Native type names accepted by `TypeNamed` remain valid.
		static bool FieldTypeNamed(std::string_view spelling, PropertyType &type, FieldPacking &packing);

		// Returns the logical field value. Native fields return an address inside
		// `component`; packed fields decode into the caller's suitably aligned
		// scratch storage and return `scratch`. The scratch must hold and align the
		// logical `PropertyType`; every currently packable type needs four bytes.
		static const void *ReadField(const void *component, const FieldDescriptor &field, void *scratch);

		// Stores one logical value, quantising and saturating when requested.
		static bool WriteField(void *component, const FieldDescriptor &field, const void *value);

		// The bytes one value of a `PropertyType` occupies.
		//
		// @param type The type.
		// @return The size, or zero for `Opaque`.
		static uint32_t SizeOf(PropertyType type);
	};
}
