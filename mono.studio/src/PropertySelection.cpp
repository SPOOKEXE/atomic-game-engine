#include <algorithm>
#include <studio/PropertySelection.hpp>
#include <utility>

namespace studio {
	using engine::core::Name;
	using engine::ecs::Classes;
	using engine::ecs::ClassId;
	using engine::ecs::Entity;
	using engine::ecs::PropertyDescriptor;
	using engine::game::PropertyValue;

	ClassId DeclaringPropertyClass(ClassId klass, Name property) {
		const engine::ecs::ClassInfo &info = Classes::Describe(klass);
		for (size_t index = info.Ancestry.size(); index > 0; index--) {
			const ClassId candidate = info.Ancestry[index - 1];
			for (const PropertyDescriptor &descriptor : Classes::Describe(candidate).Properties) {
				if (descriptor.Name == property) {
					return candidate;
				}
			}
		}
		return klass;
	}

	bool
	SelectionPropertyApplies(ClassId klass, ClassId owner, Name property, engine::ecs::PropertyType type) {
		if (!klass.IsValid() || DeclaringPropertyClass(klass, property) != owner) {
			return false;
		}
		for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
			if (descriptor.Name == property && descriptor.Type == type) {
				return true;
			}
		}
		return false;
	}

	std::vector<SelectionPropertyGroup>
	BuildPropertySelection(const engine::ecs::Store &store, std::span<const Entity> instances) {
		std::vector<SelectionPropertyGroup> groups;
		std::vector<Entity> liveInstances;
		liveInstances.reserve(instances.size());
		for (const Entity instance : instances) {
			if (store.Alive(instance) && store.ClassOf(instance).IsValid()) {
				liveInstances.push_back(instance);
			}
		}
		if (liveInstances.empty()) {
			return groups;
		}

		const ClassId primaryClass = store.ClassOf(liveInstances.front());
		const Name parent("Parent");

		for (const PropertyDescriptor &descriptor : Classes::Describe(primaryClass).Properties) {
			if (descriptor.Name == parent) {
				continue;
			}

			const ClassId owner = DeclaringPropertyClass(primaryClass, descriptor.Name);
			const bool shared = std::all_of(liveInstances.begin(), liveInstances.end(), [&](Entity instance) {
				return SelectionPropertyApplies(
					store.ClassOf(instance), owner, descriptor.Name, descriptor.Type
				);
			});
			if (!shared) {
				continue;
			}

			auto group = std::find_if(groups.begin(), groups.end(), [owner](const auto &candidate) {
				return candidate.Owner == owner;
			});
			if (group == groups.end()) {
				SelectionPropertyGroup added;
				added.Owner = owner;
				added.Applicable = liveInstances.size();
				groups.push_back(std::move(added));
				group = groups.end() - 1;
			}

			SelectionPropertyRow row;
			row.Descriptor = &descriptor;
			row.Applicable = liveInstances.size();
			for (const Entity instance : liveInstances) {
				PropertyValue value;
				if (!engine::game::ReadProperty(store, instance, descriptor, value)) {
					continue;
				}
				if (row.Readable == 0) {
					row.Value = value;
				} else if (!engine::game::ValuesEqual(row.Value, value)) {
					row.Mixed = true;
				}
				row.Readable++;
			}
			if (row.Readable != 0 && row.Readable != liveInstances.size()) {
				row.Mixed = true;
			}
			group->Rows.push_back(std::move(row));
		}

		std::sort(groups.begin(), groups.end(), [](const auto &left, const auto &right) {
			const size_t leftDepth = Classes::Describe(left.Owner).Ancestry.size();
			const size_t rightDepth = Classes::Describe(right.Owner).Ancestry.size();
			if (leftDepth != rightDepth) {
				return leftDepth < rightDepth;
			}
			return Classes::Describe(left.Owner).Name.Id() < Classes::Describe(right.Owner).Name.Id();
		});
		return groups;
	}
}
