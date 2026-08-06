#include <engine/core/Name.hpp>

#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace engine::core {

	namespace {

		struct Registry {
			// **A shared mutex, because reading a name's text is one of the
			// hottest things in the engine and needs nothing exclusive.**
			//
			// `Text()` is called on every property comparison, every log line,
			// every panel row and every save-file field. The registry it reads
			// is append-only — the deque never moves what it holds and nothing
			// is ever removed, which `Text()`'s own return comment already
			// relies on — so concurrent readers do not interfere with each
			// other at all. With a plain mutex they serialised anyway, and with
			// `ExecutionMode::WorldParallel` two worlds ticking on two workers
			// contended on this one lock for every name either of them touched.
			//
			// `Slots` is the one thing a reader indexes that a writer can
			// *reallocate*, which is why this is a shared lock rather than no
			// lock: the deque could be read unsynchronised, that vector cannot.
			std::shared_mutex Guard;

			// A deque, not a vector: references into it stay valid when it
			// grows, which is what lets Text() hand out a string_view that
			// outlives the next registration. A vector would reallocate and
			// dangle every view already given out.
			std::deque<std::string> Texts;

			// Keyed by a view into Texts, so the key costs nothing beyond the
			// string already stored. Safe for the same reason.
			std::unordered_map<std::string_view, uint32_t> Ids;

			// id -> index into Texts. Usually the identity, and not when
			// Reserve has pinned something out of order.
			std::vector<uint32_t> Slots;

			uint32_t NextId = 0;
		};

		Registry &Get() {
			static Registry registry;
			return registry;
		}

		// Caller holds the lock.
		void Bind(Registry &registry, uint32_t id, uint32_t slot) {
			if (registry.Slots.size() <= id) {
				registry.Slots.resize(static_cast<size_t>(id) + 1, Name::INVALID);
			}
			registry.Slots[id] = slot;
		}

		// The next free id, skipping anything Reserve has pinned.
		//
		// Deliberately does *not* jump past a high pin. Reserving 500'000 and
		// then setting the counter to 500'001 would make every later name take
		// a huge id — the ids stop being dense, which was the reason for having
		// a counter at all, and any further pin below the new high-water mark
		// gets consumed by ordinary interning. Skipping instead keeps both
		// properties, at the cost of a walk that only ever advances.
		uint32_t AllocateId(Registry &registry) {
			while (registry.NextId < registry.Slots.size() &&
				   registry.Slots[registry.NextId] != Name::INVALID) {
				registry.NextId++;
			}
			return registry.NextId++;
		}
	}

	Name::Name(std::string_view text) {
		if (text.empty()) {
			return;
		}

		auto &registry = Get();

		// **The hit is a read, and it is almost every call.** A process interns
		// each distinct name once and then constructs from that text for the
		// life of the run — a component name, a property name, a service name —
		// so the miss below happens a few thousand times at load and the lookup
		// here happens continuously.
		//
		// Taking the registry exclusively for that lookup is what the `Guard`
		// was made a `shared_mutex` to avoid, and it was still happening:
		// `engine.core.bench.names` measured the same total work at 23 ns a
		// lookup on one thread and 136 on eight, which is worse than serial and
		// is what full exclusion looks like. `Text()` on the same ladder went 10
		// to 30, because it was already taking the lock shared.
		{
			std::shared_lock lock(registry.Guard);

			const auto existing = registry.Ids.find(text);
			if (existing != registry.Ids.end()) {
				Identifier = existing->second;
				return;
			}
		}

		// A miss. `shared_mutex` cannot upgrade, so the shared lock is dropped
		// and an exclusive one taken — which means another thread may have
		// interned this very text in between, and the lookup has to happen
		// again. **Without the re-check two threads that both missed would both
		// insert, and one text would have two ids** — which is precisely the
		// thing `Name` exists to make impossible.
		std::lock_guard lock(registry.Guard);

		const auto existing = registry.Ids.find(text);
		if (existing != registry.Ids.end()) {
			Identifier = existing->second;
			return;
		}

		const auto slot = static_cast<uint32_t>(registry.Texts.size());
		registry.Texts.emplace_back(text);
		Identifier = AllocateId(registry);

		// The key views the stored copy, not the caller's argument, which may
		// be a temporary.
		registry.Ids.emplace(std::string_view(registry.Texts.back()), Identifier);
		Bind(registry, Identifier, slot);
	}

	Name Name::Reserve(std::string_view text, uint32_t id) {
		if (text.empty() || id == INVALID) {
			return {};
		}

		auto &registry = Get();
		std::lock_guard lock(registry.Guard);

		const auto existing = registry.Ids.find(text);
		if (existing != registry.Ids.end()) {
			// Already interned. Agreeing is fine and idempotent; disagreeing is
			// two different pins for one name, which is a startup error.
			return existing->second == id ? Name(id) : Name();
		}

		if (id < registry.Slots.size() && registry.Slots[id] != INVALID) {
			// The id is taken by another name. Pinning it anyway would make two
			// names compare equal.
			return {};
		}

		const auto slot = static_cast<uint32_t>(registry.Texts.size());
		registry.Texts.emplace_back(text);
		registry.Ids.emplace(std::string_view(registry.Texts.back()), id);
		Bind(registry, id, slot);

		return Name(id);
	}

	Name Name::FromId(uint32_t id) {
		auto &registry = Get();

		// Shared: this reads `Slots` and writes nothing. It was exclusive, which
		// meant a deserialiser resolving ids on two worlds' threads serialised
		// on a bounds check.
		std::shared_lock lock(registry.Guard);

		if (id >= registry.Slots.size() || registry.Slots[id] == INVALID) {
			return {};
		}
		return Name(id);
	}

	bool Name::Exists(std::string_view text) {
		auto &registry = Get();

		// Shared, and the header already promises it: "Whether `text` has been
		// interned, **without interning it**." A method that cannot write has no
		// business excluding the ones that only read.
		std::shared_lock lock(registry.Guard);
		return registry.Ids.find(text) != registry.Ids.end();
	}

	size_t Name::Count() {
		auto &registry = Get();
		std::shared_lock lock(registry.Guard);
		return registry.Texts.size();
	}

	std::string_view Name::Text() const {
		if (!IsValid()) {
			return {};
		}

		auto &registry = Get();
		std::shared_lock lock(registry.Guard);

		if (Identifier >= registry.Slots.size()) {
			return {};
		}
		const uint32_t slot = registry.Slots[Identifier];
		if (slot == INVALID) {
			return {};
		}

		// Safe to return past the lock: the deque never moves what it holds and
		// nothing is ever removed.
		return registry.Texts[slot];
	}
}
