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
// describe - a constructor function is neither.
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

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The value types a property can carry beyond the primitives. `ecs` is storage
// and must not know what a `Transform` is - it still has to be able to *name*
// the types userland holds, and these are `core/types` primitives rather than
// anything about a scene.
//
// **Declared rather than included, because naming is all this header does with
// them.** `TypeOf` asks `std::is_same_v`, which an incomplete type answers, and
// every caller that instantiates it holds the complete type already. The eight
// headers this replaced cost 35,742 preprocessed lines on a header 179
// translation units include, almost all of it `CFrame.hpp` reaching glm. A
// consumer that stores one of these includes `core/types/` itself.
namespace engine::core {
	struct CFrame;
	struct Color3;
	struct ColorSequence;
	struct NumberRange;
	struct NumberSequence;
	struct Rect;
	struct UDim;
	struct UDim2;
	struct Vector2;
	struct Vector3;
}

namespace engine::ecs {

	// What a property is, for a binding or an editor that has to type it.
	//
	// Deliberately small and closed. A property type that needs a case here is
	// a decision about what userland can hold, not an implementation detail -
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

		// `core::Name` - written as text, never as its process-local id.
		//
		// **For text drawn from a bounded set**, which is what interning is for:
		// a material, an asset id, a class name. Every distinct string ever
		// assigned to one of these is kept for the life of the process, so a
		// property whose value a game *computes* is the wrong shape for it - see
		// `String` below, which exists because that was found the hard way.
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

		// An owned string, stored in the component rather than interned.
		//
		// **Added at v0.8 because `Name` is a leak for text that changes**, which
		// is `D00020` and is the reason this member is not simply "the same as
		// `Name` with a different spelling". `core::Name` is a process-wide
		// registry that never releases: `label.Text = tostring(score)` at sixty
		// hertz interns a new string every frame, forever, and takes that
		// registry's mutex inside the frame loop to do it. A score counter is not
		// an exotic case - it is the first thing anybody writes.
		//
		// So the two are a real choice rather than a stylistic one, and the rule
		// is short: **a value the game picks from a set is a `Name`; a value the
		// game computes is a `String`.** A material, an asset id and a face are
		// the first; a score, a timer and a chat line are the second.
		//
		// The cost is paid where it belongs. A `String` property makes its
		// component non-trivial - an allocation per row rather than a shared id -
		// so a component holding one needs a written serialiser and does not
		// memcpy. `ecs::Column` has carried that path since v0.2 and this is the
		// first component set to use it.
		String,

		// `Entity` - a handle within this world, and meaningless outside it.
		Reference,

		// The `core/types` values a script actually holds. These describe what
		// **userland** sees, not what a component stores - a property whose
		// getter doubles a half-extent is a `Vector3` here and the storage is
		// whatever `Bounds` says. That is why this list stays short no matter
		// what the components grow: `spatial::LayerMask` will never need a case,
		// because no property is one.
		Vector3,
		CFrame,
		Color3,

		// The four a 2D tree is authored in, added at v0.8 for `gui`.
		//
		// **They are here because a `UDim2` has to be a property, not because
		// `core/types` gained four headers.** All four have existed since v0.6
		// and none of them was a `PropertyType`, so a component holding one
		// could not be saved, could not appear in a properties panel and could
		// not be set from a script - which makes `Frame.Size` unauthorable, and
		// an unauthorable size is a widget set nobody can use.
		//
		// The list stays closed and this is what growing it looks like: four
		// cases in `game::Values`, four widgets in the properties panel, four
		// in each binding, and this comment saying why.
		Vector2,
		UDim,
		UDim2,
		Rect,

		// The three a curve is authored in, added at v0.10 for `effects`.
		//
		// **Here for the reason the four above are: a value that cannot be a
		// property is a value nothing can author.** `core::NumberSequence` and
		// `core::ColorSequence` have existed since v0.6 and neither was a
		// `PropertyType`, which was fine while the only thing holding one was a
		// local in a script. A particle emitter holds eight of them, and an
		// emitter whose `Size` could not be saved, could not appear in a
		// properties panel and could not be set from a script would be an emitter
		// nobody can author - the same gap `UDim2` had at v0.8, in the same words.
		//
		// **They are big, and that is the one thing about them worth checking
		// against the closed-list rule.** A `NumberSequence` is 328 bytes and a
		// `ColorSequence` is 328; every other member of this enum is 32 or fewer.
		// `PropertyDescriptor::Size` already carries the width and every path
		// checks a caller's buffer against it, so nothing here assumes a property
		// value fits in a register - but a marshaller that copied by value into a
		// stack temporary per read is a marshaller that just grew its frame by a
		// third of a kilobyte. `control/src/Tools.cpp` reads through a shared
		// buffer for exactly this reason and is where the size is felt.
		//
		// `NumberRange` joins them because an emitter's `Lifetime` and `Speed` are
		// ranges rather than curves, and a range spelled as two float properties
		// is two writes with a frame between them where the minimum exceeds the
		// maximum.
		NumberRange,
		NumberSequence,
		ColorSequence,
	};

	// How a property reaches the components underneath it.
	//
	// Every property is a conversion - see `PropertyDescriptor` - so this does
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

		// The write lands on a world resource rather than on the instance.
		//
		// `Workspace.CurrentCamera` and `Workspace.SurfaceBounces` are the two:
		// both are read and written through an instance and neither touches a
		// row. `Reads` and `Writes` can only name components, so they name the
		// nearest one and a `.Changed` listener over-reports - which is the
		// direction `ecs::ChangeChannel` says to err in.
		//
		// **Declared rather than left implicit**, which is a correction to what
		// `Services.cpp` decided when there was one of these: a setter that
		// marks no component looks exactly like a setter that forgot to, and
		// `AuditProperties` cannot tell the two apart without being told. The
		// argument against inventing a member for a single property does not
		// survive a second property and a check that needs the distinction.
		//
		// @since v0.15
		Resource,
	};

	class Store;

	// How one property projects onto the components underneath it.
	//
	// **This used to be a component and a byte offset, and that was wrong** - it
	// could describe `Visible` and could not describe `Size`, `Position` or
	// `Anchored`, which is most of what a script reaches for. Roblox's `Size` is
	// a full extent and `Bounds::HalfExtent` is half of one; a member pointer
	// cannot express the doubling, so a member pointer was the wrong primitive.
	//
	// A property is a **conversion**: a getter that reads components and
	// produces a userland value, and a setter that takes one and writes them
	// back. A plain field is the degenerate case and its conversion is
	// generated, so there is one mechanism rather than a fast path and a slow
	// one - two would be two places to forget the change mark in.
	//
	// **The conversion cost is paid only by callers who arrived through a
	// name.** `Each<Transform, const Motion>` never sees any of this; physics
	// and render do not learn that a Roblox vocabulary exists.
	//
	// A clamp bound, carried as a template argument.
	//
	// **A `float` in everything but the one place it would stop compiling.** The
	// bounds have to be template arguments rather than parameters - see
	// `ClampedProperty` below for why the generated setter must stay captureless -
	// and a floating-point *non-type template parameter* is C++20 that GCC
	// implements and AppleClang does not. `ClampedProperty<&Sound::Volume, 0.0f,
	// 10.0f>` compiled here and stopped the macOS release build with "invalid
	// explicitly-specified argument for template parameter 'Low'".
	//
	// So this holds the IEEE-754 bits instead. An integer is a structural type on
	// every compiler, and a class type whose members are structural has been a
	// legal template argument since C++20 landed - which is a much older feature
	// than P1907 and is what makes this portable.
	//
	// **Nothing at a call site changes.** The converting constructor is
	// `constexpr`, so `0.0f` is still written as `0.0f` and the conversion happens
	// where the argument is formed. `Value()` gives the float back, and both
	// directions fold at compile time.
	struct Bound {
		// The bits of the float this stands for.
		uint32_t Bits = 0;

		// Not `explicit`, which is the point: it is what keeps the call sites
		// reading as the numbers they are.
		constexpr Bound(float value) : Bits(std::bit_cast<uint32_t>(value)) {}

		// The bound itself.
		constexpr float Value() const {
			return std::bit_cast<float>(Bits);
		}
	};

	// @since v0.2
	struct PropertyDescriptor {
		// The property's name, as scripts and files spell it.
		core::Name Name;

		// That same name's text, resolved once when the property is declared.
		//
		// **`core::Name::Text()` takes the process-wide name registry's lock**,
		// and a binding matching a script's key against a class's property list
		// calls it once per descriptor it walks - five times for `Position` on a
		// `Part`, thirteen for `CFrame`, and the whole list on a miss. At two
		// hundred property writes a frame that is over a thousand lock
		// acquisitions to compare strings that never change.
		//
		// `script/src/LuauInstances.cpp` compares keys as *text* rather than
		// interning them, and its comment records the measurement behind that:
		// interning takes the same registry lock plus a hash. That reasoning is
		// still right - what it assumed was that reading the text was free, and
		// it is not. This is what makes it free.
		//
		// Safe to hold forever: `core/src/Name.cpp` keeps its strings in a deque
		// that never moves an element and never removes one, which is the same
		// property `Text()`'s own return relies on.
		//
		// Filled by `Declare`, so every path that can produce a descriptor fills
		// it - `Property`, `ClampedProperty` and `Computed` all go through it.
		//
		// @since v0.8
		std::string_view Spelling;

		// The type of the value that crosses, not of anything stored.
		PropertyType Type = PropertyType::Opaque;

		// The size of that value, in bytes. Checked against what a caller
		// passes, so a `Vector3` handed to a `CFrame` property fails rather
		// than writing twelve bytes into twenty-eight and leaving the rest.
		uint32_t Size = 0;

		// What a write costs, and where it is legal.
		PropertyKind Kind = PropertyKind::Field;

		// False for a property that can be read and not written.
		//
		// **This flag is the whole enforcement, which is why it is worth
		// naming what checks it.** `Store::SetProperty`, both script bindings
		// and the properties panel each refuse on this alone, and the bindings
		// generator turns it into `readonly` in TypeScript and `read` in Luau -
		// so a script that tries is stopped at typecheck rather than at run
		// time. A read-only property therefore needs no `Set`, and leaving that
		// pointer null is the second refusal for a caller that reached past the
		// flag.
		//
		// `Services::LocalPlayer` (who you are is not yours to assign), the
		// `GuiObject` absolutes (derived from a layout pass) and
		// `MeshPart::TrianglesCount` (a fact about the mesh, which a publisher
		// owns) are the three today.
		bool Writable = true;

		// False for a property a *script* may not touch, in either direction.
		//
		// **A different question from `Writable`, and the difference is who is
		// asking.** `Writable` is about the value: a mass derived from a density
		// has no assignment that could mean anything, so nobody may write it -
		// not a script, not a panel, not a file. This is about the *caller*: a
		// script container's source is edited by an author every day, and a
		// script rewriting another script's source is the sandbox escape that
		// makes every other boundary in this engine decorative.
		//
		// **Enforced in the two script bindings and nowhere else.** The
		// properties panel, a game file, the Rojo sync and the control surface
		// all write through `Store::SetProperty` and are unaffected - they are
		// the author, not the program. The bindings generator leaves a
		// non-scriptable property out of the Luau and TypeScript declarations
		// entirely, so a script that reaches for one fails at typecheck rather
		// than at run time.
		//
		// Roblox draws the same line and calls it the same thing: `Source` is
		// editable in Studio and not from a game script.
		//
		// @since v0.14
		bool Scriptable = true;

		// Which set an `Enum` property's value must belong to.
		//
		// Invalid for every other type. Named rather than pointed at, so the
		// enum can be registered after the property that uses it - a class tree
		// is built by several files in whatever order the linker ran them, which
		// is the same reason `Classes` merges inherited properties lazily.
		core::Name EnumName;

		// The components the getter reads and the setter touches.
		//
		// Not decoration. The manifest reports them, a future editor needs to
		// know what dirties what, and v0.6's per-instance `.Changed` has to fan
		// one component write out to every property name observing it - writing
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
		// instead of a chain of lookups. Class trees are shallow - `Instance`,
		// `PVInstance`, `BasePart`, `Part` - so a scan beats an interval
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
		// **marked changed for free** - handing out a mutable pointer already
		// counts as a write in `StoreState`. A setter that reached the bytes any
		// other way would be a script write `replication` never sends.
		//
		// **Defined in `Property.hpp`, not here**, because the generated
		// conversion calls `Store` and this header is below it - `Store.hpp`
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
		// character `Property<Member>` plus one `std::clamp` - and the next 0..1
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
		// function pointer - `Property` is written the way it is precisely so the
		// generated setter is captureless - so bounds passed as values would need
		// a capturing lambda and would not convert. As template arguments they
		// are baked into the generated function and the descriptor stays a
		// pointer.
		//
		// They are `Bound` rather than `float` for a second reason that has
		// nothing to do with this design and everything to do with one compiler:
		// a floating-point non-type template parameter is C++20 that AppleClang
		// does not implement. `Bound` holds the bits and hands the float back,
		// and a call site is written exactly as it was.
		//
		// Usage: `ClampedProperty<&Visual::Transparency, 0.0f, 1.0f>(basePart,
		// "Transparency")`.
		//
		// @tparam Member A pointer-to-member of a registered component type. Its
		//                type must be `float`.
		// @tparam Low    The smallest legal value, written as a float.
		// @tparam High   The largest.
		// @param owner   The class that declares it.
		// @param name    The property's name.
		template <auto Member, Bound Low, Bound High>
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

		// The `Instance` root, with `Name` and `Parent` on it.
		//
		// **Here rather than in whichever module registers a tree first**, and
		// that is the same correction `ecs.Hierarchy` already went through:
		// `InstanceName` and `Hierarchy` are this module's own types, so the two
		// properties projecting them are this module's to declare. `scene` owned
		// them until v0.8, which was fine while `scene` was the only class tree
		// - and stopped being fine the moment `gui` needed the same root without
		// being allowed to link `scene`.
		//
		// Idempotent, like every registration here: the second caller gets the
		// same id, and the two property declarations are one declaration rather
		// than two that agree until somebody edits one.
		//
		// @return The `Instance` class id.
		static ClassId RegisterInstanceRoot();

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
		// After this, registering a class that has no id aborts - for the same
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
		// answer. Not a failure - a component may hold something userland never
		// sees, and the storage still handles it.
		template <class T> static constexpr PropertyType TypeOf() {
			using Bare = std::remove_cv_t<T>;
			if constexpr (std::is_same_v<Bare, core::Vector3>) {
				return PropertyType::Vector3;
			} else if constexpr (std::is_same_v<Bare, core::CFrame>) {
				return PropertyType::CFrame;
			} else if constexpr (std::is_same_v<Bare, core::Color3>) {
				return PropertyType::Color3;
			} else if constexpr (std::is_same_v<Bare, core::Vector2>) {
				return PropertyType::Vector2;
			} else if constexpr (std::is_same_v<Bare, core::UDim>) {
				return PropertyType::UDim;
			} else if constexpr (std::is_same_v<Bare, core::UDim2>) {
				return PropertyType::UDim2;
			} else if constexpr (std::is_same_v<Bare, core::Rect>) {
				return PropertyType::Rect;
			} else if constexpr (std::is_same_v<Bare, core::NumberRange>) {
				return PropertyType::NumberRange;
			} else if constexpr (std::is_same_v<Bare, core::NumberSequence>) {
				return PropertyType::NumberSequence;
			} else if constexpr (std::is_same_v<Bare, core::ColorSequence>) {
				return PropertyType::ColorSequence;
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
			} else if constexpr (std::is_same_v<Bare, std::string>) {
				return PropertyType::String;
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
