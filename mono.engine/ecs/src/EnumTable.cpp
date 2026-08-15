#include <engine/ecs/EnumTable.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	namespace {
		// One enum and its members, in registration order.
		//
		// The spans handed out have to stay valid for the life of the process,
		// so the vectors backing them live in a deque that never moves an
		// element - the same arrangement `Classes` uses and for the same reason.
		struct Entry {
			core::Name Name;
			std::vector<core::Name> Members;
		};

		struct Table {
			std::mutex Guard;
			std::deque<Entry> Entries;
			std::unordered_map<uint32_t, size_t> ByName;

			// Every registered enum's name, so `Names` can hand back a span
			// rather than build one per call.
			std::vector<core::Name> Order;
		};

		Table &Registry() {
			static Table table;
			return table;
		}

		// Caller holds the lock.
		Entry *Lookup(Table &table, core::Name name) {
			const auto found = table.ByName.find(name.Id());
			return found == table.ByName.end() ? nullptr : &table.Entries[found->second];
		}
	}

	void EnumTable::Register(std::string_view enumName, std::string_view member) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		const core::Name name(enumName);
		Entry *entry = Lookup(table, name);

		if (entry == nullptr) {
			table.ByName.emplace(name.Id(), table.Entries.size());
			table.Entries.push_back(Entry{name, {}});
			table.Order.push_back(name);
			entry = &table.Entries.back();
		}

		// Registering the same member twice is agreement, not conflict. Two
		// modules may each declare that `Material` has a `Plastic`, and refusing
		// the second would make the order two files happened to link in decide
		// whether a build works.
		const core::Name value(member);
		if (std::find(entry->Members.begin(), entry->Members.end(), value) == entry->Members.end()) {
			entry->Members.push_back(value);
		}
	}

	void EnumTable::Register(std::string_view enumName, std::span<const std::string_view> members) {
		for (const std::string_view member : members) {
			Register(enumName, member);
		}
	}

	bool EnumTable::Has(core::Name enumName, core::Name member) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		const Entry *entry = Lookup(table, enumName);
		if (entry == nullptr) {
			return false;
		}
		return std::find(entry->Members.begin(), entry->Members.end(), member) != entry->Members.end();
	}

	bool EnumTable::Known(core::Name enumName) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		return Lookup(table, enumName) != nullptr;
	}

	std::vector<core::Name> EnumTable::MembersOf(core::Name enumName) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		const Entry *entry = Lookup(table, enumName);
		return entry == nullptr ? std::vector<core::Name>{} : entry->Members;
	}

	core::Name EnumTable::MemberAt(core::Name enumName, size_t ordinal) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		const Entry *entry = Lookup(table, enumName);
		if (entry == nullptr || ordinal >= entry->Members.size()) {
			// An invalid `Name` rather than the first member. A stored ordinal
			// past the end means the component holds a value nothing registered
			// - reading it back as `Right` would make a corrupt row look like a
			// deliberate one.
			return core::Name{};
		}
		return entry->Members[ordinal];
	}

	bool EnumTable::OrdinalOf(core::Name enumName, core::Name member, size_t &ordinal) {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		const Entry *entry = Lookup(table, enumName);
		if (entry == nullptr) {
			return false;
		}

		const auto found = std::find(entry->Members.begin(), entry->Members.end(), member);
		if (found == entry->Members.end()) {
			return false;
		}

		// A scan rather than a map, for `Classes`' reason about short lists: an
		// enum here is a handful of names, and a hash keyed by two interned ids
		// would cost more to maintain than the walk it replaces.
		ordinal = static_cast<size_t>(std::distance(entry->Members.begin(), found));
		return true;
	}

	std::vector<core::Name> EnumTable::Names() {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		return table.Order;
	}

	size_t EnumTable::Count() {
		Table &table = Registry();
		const std::lock_guard lock(table.Guard);

		return table.Entries.size();
	}
}
