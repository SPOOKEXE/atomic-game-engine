#pragma once

// The process-wide table of every component type the engine knows about.
//
// One table for the whole process rather than one per world, because a
// component id has to mean the same thing in every world: a system registered
// once runs against many worlds, and a snapshot taken in one is restored into
// another. Per-world ids would make both of those a translation step.
//
// **Registration order decides iteration order, so it has to be fixed.**
// Component ids are a dense counter; an archetype is identified by its sorted
// list of them; archetypes are iterated in id order. Two runs that register the
// same types in a different order therefore visit rows in a different order,
// and a floating-point sum over those rows diverges. Registering everything
// during single-threaded startup is what pins it - which is what `Seal` is for.
//
// @tier L3 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/TypeDescriptor.hpp>

#include <cstddef>
#include <string_view>

namespace engine::ecs {

	// Interns component types and hands back dense ids.
	//
	// All static: there is one table, it lives for the process, and nothing
	// unregisters. That mirrors `core::Name`, and for the same reason - an id
	// that could be recycled would let a stale one silently name a new type.
	//
	// @since v0.2
	// @threadsafe
	class Components {
	  public:
		// Registers `T` under an explicit name, or returns its existing id.
		//
		// Prefer this to the automatic form for anything that will be written
		// to a file or a wire, because the automatic name is the compiler's
		// spelling of the type and a different compiler may spell it
		// differently.
		//
		// **Register explicitly before the type is first used.** `Of<T>()`
		// registers under the automatic name, and a later explicit
		// registration of the same type under a different name aborts rather
		// than silently leaving two names for one thing.
		//
		// @param name The stable name to register under.
		// @return The dense id for `T`.
		template <class T> static ComponentId Register(std::string_view name) {
			const core::Name key(name);
			return Adopt(key, DescribeType<T>(key), Slot<T>(), false);
		}

		// Registers `T` with an explicit name, writer and reader.
		//
		// The form to use when the raw object representation is the wrong
		// serialisation - most importantly for any component holding a
		// `core::Name`, whose id is process-local and must never reach a file.
		//
		// @param name  The stable name to register under.
		// @param write Appends `count` values to a writer.
		// @param read  Reads `count` already-constructed values back.
		// @return The dense id for `T`.
		template <class T>
		static ComponentId Register(
			std::string_view name,
			void (*write)(core::ByteWriter &, const void *, size_t),
			void (*read)(core::ByteReader &, void *, size_t)
		) {
			const core::Name key(name);
			TypeDescriptor descriptor = DescribeType<T>(key);
			descriptor.Write = write;
			descriptor.Read = read;
			descriptor.Serialisable = write != nullptr && read != nullptr;

			// The caller's writer replaces the object representation, so
			// neither padding nor a process-local id inside `T` can reach a
			// file through it. That is the whole reason this overload exists,
			// and `AuditComponents` reads this flag to know not to complain.
			descriptor.RawSerialisation = false;
			return Adopt(key, descriptor, Slot<T>(), false);
		}

		// Registers `T` under an explicit name with a compact wire form.
		//
		// The form to use for a component big enough that sending its object
		// representation to every client every tick is the bandwidth. `wire`
		// is what `replication` puts on a datagram; the file serialisation is
		// unchanged and stays lossless, which is what keeps a recording able to
		// reproduce the run it recorded.
		//
		// @param name The stable name to register under.
		// @param wire The compact, lossy form for a replication wire.
		// @return The dense id for `T`.
		template <class T> static ComponentId Register(std::string_view name, const WireFormat &wire) {
			const core::Name key(name);
			TypeDescriptor descriptor = DescribeType<T>(key);
			descriptor.Wire = wire;
			return Adopt(key, descriptor, Slot<T>(), false);
		}

		// The id for `T`, registering it under its compiler-spelled name if it
		// is not registered yet.
		//
		// The convenience path, and the reason a system can name a component
		// type without a declaration elsewhere. It is also the path that
		// **aborts once the table is sealed**: a type first seen during a tick
		// would take an id depending on which world ran first, and that is the
		// nondeterminism this table exists to prevent.
		//
		// @return The dense id for `T`.
		template <class T> static ComponentId Of() {
			// A function-local static per T, so the lookup happens once per
			// type per process and every later call is a load. Its
			// initialisation is thread-safe by the language rule, which is what
			// makes this callable from a system.
			//
			// Marked automatic, so a type somebody already registered under an
			// explicit name keeps that registration instead of this one
			// conflicting with it. The automatic name is a fallback, not a
			// claim.
			static const ComponentId id = [] {
				const core::Name key(TypeNameOf<T>());
				return Adopt(key, DescribeType<T>(key), Slot<T>(), true);
			}();
			return id;
		}

		// The id `T` already holds, registering nothing.
		//
		// **The form to reach for on a path that must not decide a type's
		// name.** `Of<T>()` registers under the compiler-spelled name when it
		// finds nothing, and that name then loses to an explicit registration
		// - but only by aborting, because a type cannot have two. So a call
		// made *before* startup names the type is a call that decides the name
		// wrongly and takes the process with it later.
		//
		// The gap is not theoretical: `Store::Destroy` asks whether the row it
		// is freeing sits in a tree, and it is reachable long before anything
		// registers `ecs.Hierarchy`. An invalid answer there is the correct
		// one - a type nothing has registered is a type no row can carry.
		//
		// @return The id, or an invalid id when `T` has not been registered.
		template <class T> static ComponentId Assigned() {
			return Slot<T>();
		}

		// Registers a type the compiler never saw.
		//
		// **The one entry point that is not a template, because its caller has
		// no `T`.** A component a game declares in a script or a data file is
		// described by its fields rather than by a struct, so `DescribeType<T>`
		// has nothing to work from and the descriptor is built by hand - see
		// `Schema.hpp`, which is the only caller and the only one that should
		// be.
		//
		// Everything after registration is unchanged: the id comes from the same
		// counter, so a described component sorts into an archetype beside a
		// declared one and no path downstream learns the difference.
		//
		// **`slot` is the identity, and the caller owns it.** Its *address* is
		// what the table compares to tell "this type again" from "a second type
		// under one name" - the same mechanism `Slot<T>()` provides for a C++
		// type. It must therefore be a distinct object per described type and
		// must outlive every use of the component.
		//
		// Aborts on a sealed table, exactly as `Of<T>()` does. A caller that can
		// report a refusal instead should ask `Sealed()` first;
		// `Schemas::Register` does.
		//
		// @param name       The stable name to register under.
		// @param descriptor Everything the storage needs to handle the type.
		// @param slot       Where this type's id is remembered, one per type.
		// @return The dense id.
		// @since v0.12
		static ComponentId
		RegisterDescribed(std::string_view name, const TypeDescriptor &descriptor, ComponentId &slot) {
			return Adopt(core::Name(name), descriptor, slot, false);
		}

		// The descriptor behind an id.
		//
		// @param id The id to describe.
		// @return The descriptor, or a default-constructed one for an invalid id.
		static const TypeDescriptor &Describe(ComponentId id);

		// The id registered under a name, without registering anything.
		//
		// This is the lookup a snapshot uses: a file carries names, and they
		// have to be turned back into the ids this process assigned.
		//
		// @param name The registered name.
		// @return The id, or an invalid id when nothing is registered under it.
		static ComponentId Find(core::Name name);

		// The number of registered types.
		//
		// @return The current registration count.
		static size_t Count();

		// Closes the table to new types.
		//
		// After this, registering a type that has no id aborts. Called once
		// startup has registered everything and before any world ticks in
		// parallel.
		static void Seal();

		// Reopens the table.
		//
		// For tests, and for a host that builds a second universe after tearing
		// one down. Not something a running simulation does.
		static void Unseal();

		// Reports whether the table is closed to new types.
		//
		// @return `true` when registering a new type would abort.
		static bool Sealed();

	  private:
		// Where one C++ type's id is remembered.
		//
		// The name table alone is not enough to keep a type to one id: it maps
		// *names* to ids, so registering one type under two names would mint
		// two, and an archetype built from one would not match a query built
		// from the other. This is the per-type half of that check.
		template <class T> static ComponentId &Slot() {
			static ComponentId slot;
			return slot;
		}

		// Registers `descriptor` under `name`, or returns the existing id.
		//
		// Not a template, so the whole table lives in one translation unit and
		// the per-type code is only the descriptor.
		//
		// @param name        The name to register under.
		// @param descriptor  The type information to record.
		// @param slot        Where this type's id is remembered across calls.
		// @param automatic   Whether `name` is the compiler-derived fallback,
		//                    which yields to an explicit registration rather
		//                    than conflicting with it.
		static ComponentId
		Adopt(core::Name name, const TypeDescriptor &descriptor, ComponentId &slot, bool automatic);
	};
}
