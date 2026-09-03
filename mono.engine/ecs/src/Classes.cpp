#include "Instances.hpp"

#include <engine/core/Log.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Column.hpp>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace engine::ecs {

	namespace {
		// One class, with the storage its ClassInfo spans point into.
		//
		// The spans in ClassInfo have to stay valid for the life of the
		// process, so the vectors backing them live here and the deque holding
		// these never moves an element.
		struct Entry {
			ClassInfo Info;
			std::vector<ClassId> Ancestry;

			// What this class declares itself, and the merged list including
			// everything inherited.
			//
			// Merged lazily rather than copied down at registration, because
			// copying made registration *order* significant: declaring a
			// property on a base after a derived class existed silently left it
			// out of the derived one, and nothing said so. A class tree is
			// built by several files in whatever order the linker ran them.
			std::vector<PropertyDescriptor> Declared;
			std::vector<PropertyDescriptor> Merged;
			uint64_t MergedAt = 0;

			// One single-row column per component in the set, holding the
			// prototype. Keyed rather than parallel to the set, because a
			// default may be declared before or after the set is final.
			std::unordered_map<uint32_t, Column> Defaults;
		};

		struct Table {
			// **A shared mutex, and the asymmetry is the whole reason.** This
			// table is written during startup and read on every property access
			// for the rest of the process - `Store::GetProperty` and
			// `SetProperty` both go through `Describe`, and a script animating a
			// scene does that hundreds of times a frame.
			//
			// With a plain mutex those reads *serialise against each other*, and
			// with `ExecutionMode::WorldParallel` two worlds ticking on two
			// workers contend on this one lock for every property either of them
			// touches. Nothing about a read needs that: the merged list a reader
			// walks is not being written while it walks.
			//
			// The one wrinkle is that `Describe` is not a pure read - it merges
			// lazily. That is handled where it is: a shared lock first, and the
			// unique one taken only on the rare pass that actually has merging
			// to do. See `Describe`.
			std::shared_mutex Guard;
			std::deque<Entry> Entries;
			std::unordered_map<uint32_t, ClassId> ByName;
			bool Closed = false;

			// Bumped whenever any class declares a property, so a merged list
			// built before that knows it is stale. One counter for the whole
			// table rather than per class: declarations happen at startup and
			// merges happen after, so the coarse version costs one rebuild.
			uint64_t Revision = 1;
		};

		Table &Get() {
			static Table table;
			return table;
		}

		const ClassInfo &Missing() {
			static const ClassInfo missing;
			return missing;
		}

		// Rebuilds the spans in an entry's ClassInfo after its vectors moved.
		void Republish(Entry &entry) {
			entry.Info.Ancestry = {entry.Ancestry.data(), entry.Ancestry.size()};
			entry.Info.Properties = {entry.Merged.data(), entry.Merged.size()};
		}

		// Merges a class's own declarations with everything above it.
		//
		// Base-first, so a derived class's declarations read as additions in
		// the order somebody wrote them, and a redeclared name replaces the
		// inherited one in place rather than appearing twice.
		void Remerge(Table &table, Entry &entry) {
			if (entry.MergedAt == table.Revision) {
				return;
			}

			entry.Merged.clear();

			// Ancestry is nearest-first, so walked backwards to start at the
			// root.
			for (auto step = entry.Ancestry.rbegin(); step != entry.Ancestry.rend(); ++step) {
				const Entry &level = table.Entries[step->Index];
				for (const PropertyDescriptor &property : level.Declared) {
					const auto existing = std::find_if(
						entry.Merged.begin(),
						entry.Merged.end(),
						[&property](const PropertyDescriptor &held) { return held.Name == property.Name; }
					);
					if (existing != entry.Merged.end()) {
						*existing = property;
					} else {
						entry.Merged.push_back(property);
					}
				}
			}

			entry.MergedAt = table.Revision;
			Republish(entry);
		}
	}

	ClassId
	Classes::Register(std::string_view name, ClassId parent, std::span<const ComponentId> components) {
		// **Before the lock, and before anything reaches for one of them.**
		// Every class carries the three instance components, and `Of<T>` a few
		// lines below would name them after the compiler if nothing had. See
		// `RegisterInstanceComponents`. Outside the lock because it takes the
		// component table's, and two locks taken in two orders is the one bug
		// worth avoiding by construction.
		RegisterInstanceComponents();

		auto &table = Get();
		const core::Name key(name);

		std::lock_guard lock(table.Guard);

		const auto found = table.ByName.find(key.Id());
		if (found != table.ByName.end()) {
			return found->second;
		}

		if (table.Closed) {
			ENGINE_ERROR(
				"class '{}' was registered after the table was sealed. Classes are "
				"declared during startup, because their ids decide archetype order.",
				key.Text()
			);
			std::abort();
		}

		if (parent.IsValid() && parent.Index >= table.Entries.size()) {
			ENGINE_ERROR("class '{}' names a parent that does not exist.", key.Text());
			std::abort();
		}

		// The set is the parent's plus the additions, which is what makes
		// inheritance set inclusion rather than a second lookup table.
		std::vector<ComponentId> members;
		if (parent.IsValid()) {
			const ComponentSet &inherited = *table.Entries[parent.Index].Info.Set;
			members.assign(inherited.Ids().begin(), inherited.Ids().end());
		}
		members.insert(members.end(), components.begin(), components.end());

		// Every instance carries these three, whatever else its class adds:
		// what it is, where it sits, and what it is called.
		members.push_back(Components::Of<InstanceClass>());
		members.push_back(Components::Of<Hierarchy>());
		members.push_back(Components::Of<InstanceName>());

		const auto id = ClassId{static_cast<uint32_t>(table.Entries.size())};
		table.Entries.emplace_back();
		Entry &entry = table.Entries.back();

		entry.Info.Name = key;
		entry.Info.Parent = parent;
		entry.Info.Set = &ComponentSet::Intern(members);

		entry.Ancestry.push_back(id);
		if (parent.IsValid()) {
			const Entry &above = table.Entries[parent.Index];
			entry.Ancestry.insert(entry.Ancestry.end(), above.Ancestry.begin(), above.Ancestry.end());
		}

		Republish(entry);
		table.ByName.emplace(key.Id(), id);
		return id;
	}

	ClassId Classes::Register(std::string_view name, std::span<const ComponentId> components) {
		return Register(name, ClassId{}, components);
	}

	void Classes::Declare(ClassId owner, const PropertyDescriptor &descriptor) {
		auto &table = Get();
		std::lock_guard lock(table.Guard);

		if (!owner.IsValid() || owner.Index >= table.Entries.size()) {
			return;
		}

		Entry &entry = table.Entries[owner.Index];

		// **Resolved once, here, because this is the only way a descriptor gets
		// into the table.** `Property`, `ClampedProperty` and `Computed` all
		// funnel through `Declare`, so filling it here means no caller can
		// produce one without it - which is what lets a binding compare against
		// it without checking whether it is set.
		PropertyDescriptor resolved = descriptor;
		resolved.Spelling = resolved.Name.Text();

		// Redeclaring a name on the same class replaces it. Redeclaring one a
		// base already has is also a replacement, but that happens in the
		// merge - this list is only what *this* class said.
		const auto existing = std::find_if(
			entry.Declared.begin(), entry.Declared.end(), [&resolved](const PropertyDescriptor &property) {
				return property.Name == resolved.Name;
			}
		);

		if (existing != entry.Declared.end()) {
			*existing = resolved;
		} else {
			entry.Declared.push_back(resolved);
		}

		// Every merged list in the table is now potentially stale, including
		// ones built for classes registered before this declaration.
		table.Revision++;
	}

	void Classes::SetDefault(ClassId owner, ComponentId component, const void *value) {
		auto &table = Get();
		std::lock_guard lock(table.Guard);

		if (!owner.IsValid() || owner.Index >= table.Entries.size() || !component.IsValid()) {
			return;
		}

		Entry &entry = table.Entries[owner.Index];
		if (!entry.Info.Set->Contains(component)) {
			ENGINE_ERROR(
				"class '{}' has no component '{}', so it cannot have a default for one.",
				entry.Info.Name.Text(),
				Components::Describe(component).Name.Text()
			);
			return;
		}

		auto found = entry.Defaults.find(component.Index);
		if (found == entry.Defaults.end()) {
			Column column(component);
			column.PushCopy(value);
			entry.Defaults.emplace(component.Index, std::move(column));
			return;
		}

		found->second.Assign(0, value);
	}

	const ClassInfo &Classes::Describe(ClassId id) {
		auto &table = Get();

		// **Two passes, and the second one almost never runs.** This is the
		// hottest read in the engine - every `Store::GetProperty` and
		// `SetProperty` goes through it, so a script animating two hundred parts
		// arrives here hundreds of times a frame, from as many threads as there
		// are worlds ticking.
		//
		// The merge it does is lazy but it is also *done*: declarations happen
		// while a class tree is being built and reads happen for the rest of the
		// process, so after startup `MergedAt == Revision` on every call and the
		// only thing the exclusive lock was buying was that comparison.
		//
		// So: take the shared lock, check, and leave. Two worlds reading at once
		// no longer serialise against each other, which with
		// `ExecutionMode::WorldParallel` was contention on one process-wide
		// mutex for every property either world touched.
		{
			std::shared_lock lock(table.Guard);

			if (!id.IsValid() || id.Index >= table.Entries.size()) {
				return Missing();
			}

			const Entry &entry = table.Entries[id.Index];
			if (entry.MergedAt == table.Revision) {
				return entry.Info;
			}
		}

		// Stale, so the merge has to happen and it is a write. **Re-checked
		// under the exclusive lock rather than assumed**, because the shared
		// lock was dropped to take this one and another thread may have merged
		// the same entry in between - `Remerge` already returns early on that,
		// which is what makes the double check free rather than a second copy of
		// the condition.
		std::unique_lock lock(table.Guard);

		if (!id.IsValid() || id.Index >= table.Entries.size()) {
			return Missing();
		}

		Entry &entry = table.Entries[id.Index];
		Remerge(table, entry);
		return entry.Info;
	}

	ClassId Classes::Find(core::Name name) {
		auto &table = Get();
		std::shared_lock lock(table.Guard);

		const auto found = table.ByName.find(name.Id());
		return found == table.ByName.end() ? ClassId{} : found->second;
	}

	void Classes::SetCreatable(ClassId id, bool creatable) {
		auto &table = Get();
		std::lock_guard lock(table.Guard);
		if (!id.IsValid() || id.Index >= table.Entries.size()) {
			return;
		}
		table.Entries[id.Index].Info.Creatable = creatable;
	}

	bool Classes::IsA(ClassId derived, ClassId base) {
		if (!derived.IsValid() || !base.IsValid()) {
			return false;
		}

		auto &table = Get();
		std::shared_lock lock(table.Guard);

		if (derived.Index >= table.Entries.size()) {
			return false;
		}

		const std::vector<ClassId> &ancestry = table.Entries[derived.Index].Ancestry;
		return std::find(ancestry.begin(), ancestry.end(), base) != ancestry.end();
	}

	const void *Classes::DefaultOf(ClassId id, ComponentId component) {
		auto &table = Get();
		std::shared_lock lock(table.Guard);

		if (!id.IsValid() || id.Index >= table.Entries.size()) {
			return nullptr;
		}

		// Walked up the ancestry rather than copied down at registration, so a
		// default declared on a base after a derived class was registered still
		// reaches it. Nearest wins, which is what lets a subclass override one.
		for (const ClassId step : table.Entries[id.Index].Ancestry) {
			const Entry &level = table.Entries[step.Index];
			const auto found = level.Defaults.find(component.Index);
			if (found != level.Defaults.end()) {
				return found->second.At(0);
			}
		}
		return nullptr;
	}

	size_t Classes::Count() {
		auto &table = Get();
		std::shared_lock lock(table.Guard);
		return table.Entries.size();
	}

	void Classes::Seal() {
		auto &table = Get();
		std::lock_guard lock(table.Guard);
		table.Closed = true;
	}

	void Classes::Unseal() {
		auto &table = Get();
		std::lock_guard lock(table.Guard);
		table.Closed = false;
	}

	bool Classes::Sealed() {
		auto &table = Get();
		std::shared_lock lock(table.Guard);
		return table.Closed;
	}

	const char *Describe(PropertyType type) {
		switch (type) {
		case PropertyType::Opaque:
			return "opaque";
		case PropertyType::Bool:
			return "bool";
		case PropertyType::Int32:
			return "int32";
		case PropertyType::Int64:
			return "int64";
		case PropertyType::Float:
			return "float";
		case PropertyType::Double:
			return "double";
		case PropertyType::Name:
			return "name";
		case PropertyType::String:
			return "string";
		case PropertyType::Enum:
			return "enum";
		case PropertyType::Reference:
			return "reference";
		case PropertyType::Vector3:
			return "Vector3";
		case PropertyType::CFrame:
			return "CFrame";
		case PropertyType::Color3:
			return "Color3";
		case PropertyType::Vector2:
			return "Vector2";
		case PropertyType::UDim:
			return "UDim";
		case PropertyType::UDim2:
			return "UDim2";
		case PropertyType::Rect:
			return "Rect";
		case PropertyType::NumberRange:
			return "NumberRange";
		case PropertyType::NumberSequence:
			return "NumberSequence";
		case PropertyType::ColorSequence:
			return "ColorSequence";
		}
		return "?";
	}
}
