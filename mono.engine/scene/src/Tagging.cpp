#include <engine/ecs/Store.hpp>
#include <engine/scene/Tagging.hpp>

#include <algorithm>

namespace engine::scene {

	uint32_t TagTable::Find(const core::Name &name) const {
		if (!name.IsValid()) {
			return 0;
		}
		const auto found = std::find(Names.begin(), Names.end(), name);
		if (found == Names.end()) {
			return 0;
		}

		// A linear scan over at most thirty-two interned names, which is
		// thirty-two integer compares. A map would be a hash and a pointer
		// chase to beat that.
		return 1u << static_cast<uint32_t>(found - Names.begin());
	}

	uint32_t TagTable::Register(const core::Name &name) {
		if (!name.IsValid()) {
			return 0;
		}
		if (const uint32_t existing = Find(name)) {
			return existing;
		}
		if (Names.size() >= MAXIMUM) {
			// Refused rather than aliased onto an existing bit. An alias means
			// one tag's objects turning up in another's pass, which is
			// invisible until somebody notices the wrong thing reflected.
			return 0;
		}

		Names.push_back(name);
		return 1u << static_cast<uint32_t>(Names.size() - 1);
	}

	std::vector<core::Name> TagTable::Describe(uint32_t mask) const {
		std::vector<core::Name> named;
		for (size_t index = 0; index < Names.size(); index++) {
			if ((mask & (1u << index)) != 0) {
				named.push_back(Names[index]);
			}
		}
		return named;
	}

	TagTable &TagsOf(ecs::Store &store) {
		if (!store.HasResource<TagTable>()) {
			store.SetResource(TagTable{});
		}
		return *store.ResourceMutable<TagTable>();
	}

	bool AddTag(ecs::Store &store, ecs::Entity entity, const core::Name &name) {
		Tags *tags = store.GetMutable<Tags>(entity);
		if (tags == nullptr) {
			return false;
		}

		// The bit is taken before the write, so a full table fails without
		// having touched the row.
		const uint32_t bit = TagsOf(store).Register(name);
		if (bit == 0) {
			return false;
		}

		tags->Mask |= bit;
		return true;
	}

	bool RemoveTag(ecs::Store &store, ecs::Entity entity, const core::Name &name) {
		Tags *tags = store.GetMutable<Tags>(entity);
		if (tags == nullptr) {
			return false;
		}

		// `Find` rather than `Register`: removing a tag a world has never heard
		// of should not put it in the table.
		tags->Mask &= ~TagsOf(store).Find(name);
		return true;
	}

	bool HasTag(const ecs::Store &store, ecs::Entity entity, const core::Name &name) {
		const Tags *tags = store.Get<Tags>(entity);
		const TagTable *table = store.Resource<TagTable>();
		if (tags == nullptr || table == nullptr) {
			return false;
		}

		const uint32_t bit = table->Find(name);
		return bit != 0 && (tags->Mask & bit) != 0;
	}
}
