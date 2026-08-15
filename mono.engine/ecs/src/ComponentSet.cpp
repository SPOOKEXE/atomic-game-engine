#include <engine/ecs/ComponentSet.hpp>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	namespace {
		// The key a set is looked up by.
		//
		// The sorted id list itself, because that *is* the identity: two
		// callers naming the same components in different orders have to reach
		// the same entry, and after sorting they have the same key.
		struct Key {
			std::vector<ComponentId> Members;

			bool operator==(const Key &other) const {
				return Members.size() == other.Members.size() &&
					   std::equal(Members.begin(), Members.end(), other.Members.begin());
			}
		};

		struct KeyHash {
			size_t operator()(const Key &key) const {
				// FNV-1a over the ids. Specified rather than borrowed from a
				// standard hash so that the table behaves the same on every
				// standard library - not for determinism of results, which does
				// not depend on it, but so a pathological bucket distribution
				// is reproducible when somebody profiles one.
				size_t hash = 1469598103934665603ull;
				for (const ComponentId id : key.Members) {
					hash ^= id.Index;
					hash *= 1099511628211ull;
				}
				return hash;
			}
		};
	}

	// Owns every interned set and hands out stable references to them.
	//
	// Named rather than anonymous because ComponentSet befriends it: the sets
	// it builds need their private fields filled in, and a friend declaration
	// is a smaller hole than public setters nobody else should ever call.
	class ComponentSetTable {
	  public:
		static ComponentSetTable &Get() {
			static ComponentSetTable table;
			return table;
		}

		const ComponentSet &Intern(std::span<const ComponentId> ids) {
			Key key;
			key.Members.reserve(ids.size());
			for (const ComponentId id : ids) {
				// A set holding an invalid id would describe an archetype with
				// a column of nothing. Dropping it is what lets a caller
				// resolve names out of a snapshot this build does not fully
				// understand and still get a usable set.
				if (id.IsValid()) {
					key.Members.push_back(id);
				}
			}

			std::sort(key.Members.begin(), key.Members.end());
			key.Members.erase(std::unique(key.Members.begin(), key.Members.end()), key.Members.end());

			std::lock_guard lock(Guard);

			const auto found = ByMembers.find(key);
			if (found != ByMembers.end()) {
				return *found->second;
			}

			// A deque for both, so that a reference or a span handed out stays
			// valid while later sets are interned. A vector would move the
			// storage out from under every archetype holding one.
			Storage.push_back(key.Members);

			// Constructed here rather than emplaced into the deque, because the
			// constructor is private and friendship does not reach the
			// allocator that a container would construct through. `new` inside
			// this member does.
			Sets.push_back(std::unique_ptr<ComponentSet>(new ComponentSet()));

			ComponentSet &set = *Sets.back();
			set.Identifier = static_cast<uint32_t>(Sets.size() - 1);
			set.Members = {Storage.back().data(), Storage.back().size()};

			ByMembers.emplace(std::move(key), &set);
			return set;
		}

		size_t Count() {
			std::lock_guard lock(Guard);
			return Sets.size();
		}

	  private:
		std::mutex Guard;
		std::deque<std::unique_ptr<ComponentSet>> Sets;
		std::deque<std::vector<ComponentId>> Storage;
		std::unordered_map<Key, ComponentSet *, KeyHash> ByMembers;
	};

	const ComponentSet &ComponentSet::Intern(std::span<const ComponentId> ids) {
		return ComponentSetTable::Get().Intern(ids);
	}

	const ComponentSet &ComponentSet::Intern(std::initializer_list<ComponentId> ids) {
		return ComponentSetTable::Get().Intern({ids.begin(), ids.size()});
	}

	const ComponentSet &ComponentSet::Empty() {
		// Interned like any other, so the empty set has an id and an archetype
		// can name it. An entity with no components is not a special case.
		static const ComponentSet &empty = ComponentSetTable::Get().Intern({});
		return empty;
	}

	size_t ComponentSet::Count() {
		return ComponentSetTable::Get().Count();
	}

	bool ComponentSet::Contains(ComponentId id) const {
		return std::binary_search(Members.begin(), Members.end(), id);
	}

	bool ComponentSet::ContainsAll(std::span<const ComponentId> ids) const {
		for (const ComponentId id : ids) {
			if (!id.IsValid() || !Contains(id)) {
				return false;
			}
		}
		return true;
	}

	const ComponentSet &ComponentSet::With(ComponentId id) const {
		if (!id.IsValid() || Contains(id)) {
			return *this;
		}

		std::vector<ComponentId> members(Members.begin(), Members.end());
		members.push_back(id);
		return Intern(members);
	}

	const ComponentSet &ComponentSet::Without(ComponentId id) const {
		if (!Contains(id)) {
			return *this;
		}

		std::vector<ComponentId> members;
		members.reserve(Members.size() - 1);
		for (const ComponentId member : Members) {
			if (member != id) {
				members.push_back(member);
			}
		}
		return Intern(members);
	}
}
