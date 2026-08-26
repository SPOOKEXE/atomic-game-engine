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
		const Name parent("Parent");

		for (const Entity instance : instances) {
			if (!store.Alive(instance)) {
				continue;
			}
			const ClassId klass = store.ClassOf(instance);
			if (!klass.IsValid()) {
				continue;
			}

			std::vector<ClassId> ownersSeen;
			for (const PropertyDescriptor &descriptor : Classes::Describe(klass).Properties) {
				if (descriptor.Name == parent) {
					continue;
				}

				const ClassId owner = DeclaringPropertyClass(klass, descriptor.Name);
				auto group = std::find_if(groups.begin(), groups.end(), [owner](const auto &candidate) {
					return candidate.Owner == owner;
				});
				if (group == groups.end()) {
					SelectionPropertyGroup added;
					added.Owner = owner;
					groups.push_back(std::move(added));
					group = groups.end() - 1;
				}
				if (std::find(ownersSeen.begin(), ownersSeen.end(), owner) == ownersSeen.end()) {
					group->Applicable++;
					ownersSeen.push_back(owner);
				}

				auto row = std::find_if(group->Rows.begin(), group->Rows.end(), [&](const auto &candidate) {
					return candidate.Descriptor != nullptr && candidate.Descriptor->Name == descriptor.Name &&
						   candidate.Descriptor->Type == descriptor.Type;
				});
				if (row == group->Rows.end()) {
					SelectionPropertyRow added;
					added.Descriptor = &descriptor;
					group->Rows.push_back(std::move(added));
					row = group->Rows.end() - 1;
				}

				row->Applicable++;
				PropertyValue value;
				if (!engine::game::ReadProperty(store, instance, descriptor, value)) {
					continue;
				}
				if (row->Readable == 0) {
					row->Value = value;
				} else if (!engine::game::ValuesEqual(row->Value, value)) {
					row->Mixed = true;
				}
				row->Readable++;
			}
		}

		for (SelectionPropertyGroup &group : groups) {
			for (SelectionPropertyRow &row : group.Rows) {
				if (row.Readable != 0 && row.Readable != row.Applicable) {
					row.Mixed = true;
				}
			}
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
