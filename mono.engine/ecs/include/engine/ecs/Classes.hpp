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

		// `Entity` — a handle within this world, and meaningless outside it.
		Reference,
	};

	// Where one property lives.
	//
	// A component and a byte offset into it, which is all a runtime setter
	// needs: `part.Size = x` resolves a name to one of these once and then
	// writes at an address.
	//
	// @since v0.2
	struct PropertyDescriptor {
		// The property's name, as scripts and files spell it.
		core::Name Name;

		// The component holding it.
		ComponentId Component;

		// The byte offset within that component's value.
		uint32_t Offset = 0;

		// How to interpret the bytes at that offset.
		PropertyType Type = PropertyType::Opaque;

		// The number of bytes the property occupies.
		uint32_t Size = 0;
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

		// Declares a property, resolving its offset from a member pointer.
		//
		// The offset is measured rather than declared, so a field reordered in
		// the struct does not silently point a binding at the wrong bytes.
		//
		// @param owner  The class exposing the property.
		// @param name   The name scripts and files use.
		// @param member A pointer to the member within its component type.
		template <class Component, class Member>
		static void Property(ClassId owner, std::string_view name, Member Component::*member) {
			// Measured against a real object rather than a null pointer, which
			// is the usual trick and is undefined. A default-constructed
			// component costs nothing here — this runs once, at startup.
			const Component probe{};
			const auto *base = reinterpret_cast<const std::byte *>(&probe);
			const auto *field = reinterpret_cast<const std::byte *>(&(probe.*member));

			PropertyDescriptor descriptor;
			descriptor.Name = core::Name(name);
			descriptor.Component = Components::Of<Component>();
			descriptor.Offset = static_cast<uint32_t>(field - base);
			descriptor.Type = TypeOf<Member>();
			descriptor.Size = static_cast<uint32_t>(sizeof(Member));

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
		// The property type for a C++ type, or Opaque when there is no better
		// answer. Not a failure — a component may hold something userland never
		// sees, and the storage still handles it.
		template <class T> static constexpr PropertyType TypeOf() {
			using Bare = std::remove_cv_t<T>;
			if constexpr (std::is_same_v<Bare, bool>) {
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
