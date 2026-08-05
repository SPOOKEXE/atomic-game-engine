#pragma once

// The class table: what `Instance.new("Part")` resolves to.
//
// A class is three things and nothing else:
//
// - a **name**, which is what crosses a save file, a wire and a script,
// - a **`ComponentSet`**, which is the archetype its instances land in,
// - a **prototype row**, one hidden value per component, holding the defaults.
//
// The prototype is what makes `Instance.new` a column copy rather than a
// constructor call, and it is why `:Clone()` needs no separate machinery: both
// are the same copy from a different source row. It is also the only form a
// default can take that a snapshot can carry and the v0.5 bindings manifest can
// describe — a constructor function is neither.
//
// **Inheritance is set inclusion.** A derived class's set contains its base's,
// so `:IsA` is an ancestor test and a query for the base matches every derived
// instance without knowing they exist. That falls out of the storage rather
// than being built on top of it.
//
// Process-wide, like `Components`, and for the same reason: a class has to mean
// the same thing in every world, because a snapshot taken in one is restored
// into another.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
// The three value types a property can carry beyond the primitives. `ecs` is
// storage and must not know what a `Transform` is — it still has to be able to
// *name* the types userland holds, and these are `core/types` primitives rather
// than anything about a scene. Nothing here reads a field of one.
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace engine::ecs {

	// What a property is, for a binding or an editor that has to type it.
	//
	// Deliberately small and closed. A property type that needs a case here is
	// a decision about what userland can hold, not an implementation detail —
	// so adding one is a visible change rather than a template specialisation
	// somebody slips in.
	//
	// @since v0.2
	enum class PropertyType : uint8_t {
		// Anything with no better description. Readable as bytes, not as a value.
		Opaque,

		Bool,
		Int32,
		Int64,
		Float,
		Double,

		// `core::Name` — written as text, never as its process-local id.
		Name,

		// A `core::Name` that must be one of a registered set.
		//
		// **The storage is identical to `Name` and the contract is not.** A
		// `Name` property takes any string at all, so `part.Material =
		// "Plsatic"` lands in the component and surfaces as a part rendering
		// with the default material for reasons nobody can see. An `Enum`
		// property is checked against `EnumTable`, so the typo is refused where
		// it was made, and a binding can offer completion because the set is
		// readable at run time.
		//
		// Which set is named by `PropertyDescriptor::EnumName`. Adding this was
		// a decision about what userland can hold rather than an implementation
		// detail, which is exactly why `PropertyType` is a closed list.
		Enum,

		// `Entity` — a handle within this world, and meaningless outside it.
		Reference,

		// The `core/types` values a script actually holds. These describe what
		// **userland** sees, not what a component stores — a property whose
		// getter doubles a half-extent is a `Vector3` here and the storage is
		// whatever `Bounds` says. That is why this list stays short no matter
		// what the components grow: `spatial::LayerMask` will never need a case,
		// because no property is one.
		Vector3,
		CFrame,
		Color3,
	};

	// How a property reaches the components underneath it.
	//
	// Every property is a conversion — see `PropertyDescriptor` — so this does
	// not select a *mechanism*. It tells a caller what a write is going to cost
	// and whether it is allowed where they are standing.
	//
	// @since v0.5
	enum class PropertyKind : uint8_t {
		// The getter copies a field out and the setter copies one in. The
		// conversion is generated rather than written.
		Field,

		// Arithmetic, a sub-range or a lookup. `Size` is a doubled half-extent;
		// `Position` is the translation of a `CFrame` with the rotation kept.
		Computed,

		// The write changes which components the entity has, so it moves the row
		// to another archetype. `Anchored` is the only one today. Cannot happen
		// inline during iteration and goes through the deferral queue.
		Structural,
	};

	class Store;

	// How one property projects onto the components underneath it.
	//
	// **This used to be a component and a byte offset, and that was wrong** — it
	// could describe `Visible` and could not describe `Size`, `Position` or
	// `Anchored`, which is most of what a script reaches for. Roblox's `Size` is
	// a full extent and `Bounds::HalfExtent` is half of one; a member pointer
	// cannot express the doubling, so a member pointer was the wrong primitive.
	//
	// A property is a **conversion**: a getter that reads components and
	// produces a userland value, and a setter that takes one and writes them
	// back. A plain field is the degenerate case and its conversion is
	// generated, so there is one mechanism rather than a fast path and a slow
	// one — two would be two places to forget the change mark in.
	//
	// **The conversion cost is paid only by callers who arrived through a
	// name.** `Each<Transform, const Motion>` never sees any of this; physics
	// and render do not learn that a Roblox vocabulary exists.
	//
	// @since v0.2
	struct PropertyDescriptor {
		// The property's name, as scripts and files spell it.
		core::Name Name;

		// The type of the value that crosses, not of anything stored.
		PropertyType Type = PropertyType::Opaque;

		// The size of that value, in bytes. Checked against what a caller
		// passes, so a `Vector3` handed to a `CFrame` property fails rather
		// than writing twelve bytes into twenty-eight and leaving the rest.
		uint32_t Size = 0;

		// What a write costs, and where it is legal.
		PropertyKind Kind = PropertyKind::Field;

		// False for a property that can be read and not written. None today;
		// the field exists because the manifest has to be able to say it.
		bool Writable = true;

		// Which set an `Enum` property's value must belong to.
		//
		// Invalid for every other type. Named rather than pointed at, so the
		// enum can be registered after the property that uses it — a class tree
		// is built by several files in whatever order the linker ran them, which
		// is the same reason `Classes` merges inherited properties lazily.
		core::Name EnumName;

		// The components the getter reads and the setter touches.
		//
		// Not decoration. The manifest reports them, a future editor needs to
		// know what dirties what, and v0.6's per-instance `.Changed` has to fan
		// one component write out to every property name observing it — writing
		// `Transform` fires `CFrame`, `Position` *and* `Orientation`.
		const ComponentSet *Reads = nullptr;

		// The components the setter touches. The same set as `Reads` for a
		// plain field, and deliberately a separate pointer: a computed property
		// may read more than it writes, and a caller asking what a write
		// dirties must not be told what a read needed.
		const ComponentSet *Writes = nullptr;

		// Reads the property into `out`, which holds `Size` bytes.
		//
		// @return `false` when the entity does not have what the getter needs.
		bool (*Get)(const Store &store, Entity instance, void *out) = nullptr;

		// Writes `value`, which holds `Size` bytes.
		//
		// @return `false` when the entity cannot take it.
		bool (*Set)(Store &store, Entity instance, const void *value) = nullptr;
	};

	// Everything registered about one class.
	//
	// @since v0.2
	struct ClassInfo {
		// The stable identity, and what a file or a script carries.
		core::Name Name;

		// The class this one derives from, or invalid for a root.
		ClassId Parent;

		// This class and every ancestor, nearest first.
		//
		// Stored rather than walked, so `:IsA` is a scan of a handful of ids
		// instead of a chain of lookups. Class trees are shallow — `Instance`,
		// `PVInstance`, `BasePart`, `Part` — so a scan beats an interval
		// numbering that would have to be recomputed on every registration.
		std::span<const ClassId> Ancestry;

		// The full component set, including everything inherited.
		const ComponentSet *Set = nullptr;

		// The properties this class exposes, including inherited ones.
		std::span<const PropertyDescriptor> Properties;
	};

	// Registers classes and answers questions about them.
	//
	// @since v0.2
	// @threadsafe
	class Classes {
	  public:
		// Registers a class deriving from `parent` and adding `components`.
		//
		// The resulting set is the parent's plus the additions, so a derived
		// class is always a superset of its base and `:IsA` needs no separate
		// table. Registering the same name twice returns the existing id.
		//
		// @param name       The stable name.
		// @param parent     The base class, or an invalid id for a root.
		// @param components The components this class adds to its parent's set.
		// @return The class id.
		static ClassId
		Register(std::string_view name, ClassId parent, std::span<const ComponentId> components);

		// Registers a root class adding `components`.
		//
		// @param name       The stable name.
		// @param components The components this class holds.
		// @return The class id.
		static ClassId Register(std::string_view name, std::span<const ComponentId> components);

		// Declares a plain field as a property, generating its conversion.
		//
		// The member pointer is a **template argument** rather than a runtime
		// one, which is what lets the generated getter and setter be ordinary
		// function pointers with nothing captured. That is the whole reason the
		// signature changed at v0.5: a captureless conversion is what makes one
		// mechanism cover fields and computed properties alike.
		//
		//     Classes::Property<&Visual::Visible>(part, "Visible");
		//
		// The field is reached through `Store::GetMutable`, so a write is
		// **marked changed for free** — handing out a mutable pointer already
		// counts as a write in `StoreState`. A setter that reached the bytes any
		// other way would be a script write `replication` never sends.
		//
		// **Defined in `Property.hpp`, not here**, because the generated
		// conversion calls `Store` and this header is below it — `Store.hpp`
		// does not include this one and must not have to. Include
		// `engine/ecs/Property.hpp` to declare a property; include this header
		// to read one.
		//
		// @tparam Member A pointer-to-member of a registered component type.
		// @param owner   The class exposing the property.
		// @param name    The name scripts and files use.
		template <auto Member> static void Property(ClassId owner, std::string_view name);

		// The same, for a field whose legal values are a closed range.
		//
		// **Because `PropertyDescriptor` could not say "this is a fraction", and
		// the first two properties that needed to say it abandoned the generated
		// form to do it.** Each became thirty-odd lines that were character for
		// character `Property<Member>` plus one `std::clamp` — and the next 0..1
		// float would have been a third copy.
		//
		// **Clamped rather than refused**, which is the right answer for a
		// continuous quantity: a fade driven off a sine or a distance overshoots
		// by a hair at both ends, and refusing the write would make a smooth fade
		// stutter at exactly the two values it is aiming for. An enum is refused
		// because a wrong name means nothing; a number out of range has an
		// obvious nearest meaning.
		//
		// **The bounds are template arguments, not parameters, and that is
		// forced rather than stylistic.** `PropertyDescriptor::Set` is a raw
		// function pointer — `Property` is written the way it is precisely so the
		// generated setter is captureless — so bounds passed as values would need
		// a capturing lambda and would not convert. As template arguments they
		// are baked into the generated function and the descriptor stays a
		// pointer.
		//
		// Usage: `ClampedProperty<&Visual::Transparency, 0.0f, 1.0f>(basePart,
		// "Transparency")`.
		//
		// @tparam Member A pointer-to-member of a registered component type.
		// @tparam Low    The smallest legal value.
		// @tparam High   The largest.
		// @param owner   The class that declares it.
		// @param name    The property's name.
		template <auto Member, auto Low, auto High>
		static void ClampedProperty(ClassId owner, std::string_view name);

		// Declares a property whose conversion is written rather than generated.
		//
		// For everything a field cannot express: a doubled half-extent, the
		// translation of a `CFrame` with its rotation kept, a name over a bit
		// mask, a flag that is really an archetype move.
		//
		// @param owner      The class exposing the property.
		// @param descriptor The full description, conversions included.
		static void Computed(ClassId owner, const PropertyDescriptor &descriptor) {
			Declare(owner, descriptor);
		}

		// The defaults a class's instances start from.
		//
		// Setting a default before any instance exists is the ordinary case.
		// Setting one afterwards changes what *later* instances get and leaves
		// existing ones alone, because a default is a starting value rather
		// than a rule.
		//
		// @param owner The class whose prototype to write.
		// @param value The value later instances start their `T` from.
		template <class T> static void Default(ClassId owner, const T &value) {
			SetDefault(owner, Components::Of<T>(), &value);
		}

		// Everything registered about a class.
		//
		// @param id The class to describe.
		// @return The information, or an empty record for an invalid id.
		static const ClassInfo &Describe(ClassId id);

		// The class registered under a name.
		//
		// @param name The registered name.
		// @return The id, or an invalid id when nothing is registered under it.
		static ClassId Find(core::Name name);

		// Reports whether `derived` is `base` or descends from it.
		//
		// @param derived The class to test.
		// @param base    The class to test against.
		// @return `true` when derived is base or one of its descendants.
		static bool IsA(ClassId derived, ClassId base);

		// The prototype value for one component of a class.
		//
		// @param id        The class.
		// @param component The component whose default is wanted.
		// @return The value, or `nullptr` when the class has no such component.
		static const void *DefaultOf(ClassId id, ComponentId component);

		// The number of registered classes.
		//
		// @return The current registration count.
		static size_t Count();

		// Closes the table to new classes.
		//
		// After this, registering a class that has no id aborts — for the same
		// reason `Components::Seal` exists: a class first seen during a tick
		// would take an id decided by whichever world ran first.
		static void Seal();

		// Reopens the table.
		static void Unseal();

		// Reports whether the table is closed to new classes.
		//
		// @return `true` when registering a new class would abort.
		static bool Sealed();

	  private:
		// Splits a pointer-to-member into the class holding it and the type it
		// has. Only needed because `Property` takes the member as a template
		// argument, which is what makes its generated conversion captureless.
		template <class T> struct MemberOf;
		template <class C, class M> struct MemberOf<M C::*> {
			using Class = C;
			using Type = M;
		};

		// The property type for a C++ type, or Opaque when there is no better
		// answer. Not a failure — a component may hold something userland never
		// sees, and the storage still handles it.
		template <class T> static constexpr PropertyType TypeOf() {
			using Bare = std::remove_cv_t<T>;
			if constexpr (std::is_same_v<Bare, core::Vector3>) {
				return PropertyType::Vector3;
			} else if constexpr (std::is_same_v<Bare, core::CFrame>) {
				return PropertyType::CFrame;
			} else if constexpr (std::is_same_v<Bare, core::Color3>) {
				return PropertyType::Color3;
			} else if constexpr (std::is_same_v<Bare, bool>) {
				return PropertyType::Bool;
			} else if constexpr (std::is_same_v<Bare, int32_t> || std::is_same_v<Bare, uint32_t>) {
				return PropertyType::Int32;
			} else if constexpr (std::is_same_v<Bare, int64_t> || std::is_same_v<Bare, uint64_t>) {
				return PropertyType::Int64;
			} else if constexpr (std::is_same_v<Bare, float>) {
				return PropertyType::Float;
			} else if constexpr (std::is_same_v<Bare, double>) {
				return PropertyType::Double;
			} else if constexpr (std::is_same_v<Bare, core::Name>) {
				return PropertyType::Name;
			} else if constexpr (std::is_same_v<Bare, Entity>) {
				return PropertyType::Reference;
			} else {
				return PropertyType::Opaque;
			}
		}

		static void Declare(ClassId owner, const PropertyDescriptor &descriptor);
		static void SetDefault(ClassId owner, ComponentId component, const void *value);
	};

	// Returns a stable, human-readable name for a property type.
	//
	// @param type The type to name.
	// @return A view valid for the lifetime of the process.
	const char *Describe(PropertyType type);
}
