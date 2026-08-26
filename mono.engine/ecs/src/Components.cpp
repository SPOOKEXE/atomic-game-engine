#include <engine/core/Log.hpp>
#include <engine/ecs/Components.hpp>

#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace engine::ecs {

	namespace {
		// What a name resolves to, and which C++ type claimed it.
		//
		// The owner is the address of that type's slot, which is a distinct
		// static per type - so comparing it is an exact answer to "is this the
		// same type", where comparing size and alignment is only a guess.
		//
		// The case that made this necessary: two types declared in anonymous
		// namespaces in different translation units both spell as
		// `{anonymous}::Label`, so the second silently adopted the first's
		// descriptor. Every lifetime hook then belonged to the wrong type, and
		// the only symptom was a leak count that did not add up.
		struct TypeEntry {
			ComponentId Id;
			const void *Owner = nullptr;
		};

		struct TypeRegistry {
			std::mutex Guard;

			// A deque rather than a vector because Describe hands back a
			// reference and registration continues afterwards. A vector would
			// reallocate and turn every descriptor anyone was holding into a
			// dangling pointer, at a moment - startup - when nothing is looking
			// for that kind of bug.
			std::deque<TypeDescriptor> Descriptors;
			std::unordered_map<uint32_t, TypeEntry> ByName;
			bool Closed = false;
		};

		// Never destroyed, deliberately.
		//
		// A `Column` reaches its descriptor to destroy its rows, and a store held
		// in a static outlives this registry under reverse destruction order -
		// the registry is built on the first *registration*, which happens after
		// whatever static owns the store was constructed. The result is a
		// destructor reading a freed `std::deque` and calling whatever the bytes
		// there look like: it showed up as a benchmark binary that printed its
		// whole report and then segfaulted, and it depends on which static was
		// touched first, so it appears and disappears with unrelated changes.
		// The process reclaims the memory.
		TypeRegistry &Types() {
			static TypeRegistry *registry = new TypeRegistry();
			return *registry;
		}

		// Returned for an id nobody registered, so that a caller reading a
		// corrupt snapshot gets an empty descriptor rather than a bad index.
		const TypeDescriptor &MissingDescriptor() {
			static const TypeDescriptor missing;
			return missing;
		}
	}

	ComponentId
	Components::Adopt(core::Name name, const TypeDescriptor &descriptor, ComponentId &slot, bool automatic) {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);

		// The per-type check comes first, because the name table cannot make
		// it: one type registered under two names would take two ids there, and
		// an archetype built from one would silently not match a query built
		// from the other.
		if (slot.IsValid()) {
			const core::Name registered = registry.Descriptors[slot.Index].Name;
			if (automatic || registered == name) {
				return slot;
			}

			ENGINE_ERROR(
				"component '{}' is already registered as '{}'. A type has one name; "
				"register it explicitly before anything uses it.",
				name.Text(),
				registered.Text()
			);
			std::abort();
		}

		const auto found = registry.ByName.find(name.Id());
		if (found != registry.ByName.end()) {
			// Two different types under one name. Aborting rather than
			// tolerating it, because the alternative is that one of them runs
			// with the other's constructor, destructor and serialiser - which
			// presents as a leak, a double free, or a snapshot whose columns
			// line up by name and hold something else.
			//
			// The realistic cause is two anonymous-namespace types in different
			// files sharing a spelling. The fix is a `Register<T>` with a name
			// somebody chose, which is what the header recommends anyway.
			if (found->second.Owner != &slot) {
				ENGINE_ERROR(
					"component '{}' is already registered by a different type. Two "
					"types cannot share a name - register at least one of them "
					"explicitly, under a name of its own.",
					name.Text()
				);
				std::abort();
			}

			slot = found->second.Id;
			return slot;
		}

		if (registry.Closed) {
			// A type first seen after sealing would take an id decided by
			// whichever world happened to run first. Every archetype built from
			// it, and therefore every iteration order downstream, would depend
			// on thread scheduling.
			ENGINE_ERROR(
				"component '{}' was registered after the table was sealed. Every "
				"component type must be registered during startup, because "
				"registration order fixes iteration order.",
				name.Text()
			);
			std::abort();
		}

		const ComponentId id{static_cast<uint32_t>(registry.Descriptors.size())};
		registry.Descriptors.push_back(descriptor);
		registry.ByName.emplace(name.Id(), TypeEntry{id, &slot});
		slot = id;

		return id;
	}

	const TypeDescriptor &Components::Describe(ComponentId id) {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);

		if (!id.IsValid() || id.Index >= registry.Descriptors.size()) {
			return MissingDescriptor();
		}
		return registry.Descriptors[id.Index];
	}

	ComponentId Components::Find(core::Name name) {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);

		const auto found = registry.ByName.find(name.Id());
		return found == registry.ByName.end() ? ComponentId{} : found->second.Id;
	}

	size_t Components::Count() {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);
		return registry.Descriptors.size();
	}

	void Components::Seal() {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);
		registry.Closed = true;
	}

	void Components::Unseal() {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);
		registry.Closed = false;
	}

	bool Components::Sealed() {
		auto &registry = Types();
		std::lock_guard lock(registry.Guard);
		return registry.Closed;
	}
}
