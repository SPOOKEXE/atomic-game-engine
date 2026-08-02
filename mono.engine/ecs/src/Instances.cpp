#include "Instances.hpp"

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Components.hpp>

#include <vector>

namespace engine::ecs {

	namespace {
		// One node's tree links, or null when the entity is not an instance.
		const Hierarchy *NodeOf(const StoreState &state, Entity instance) {
			return static_cast<const Hierarchy *>(GetComponent(state, instance, Components::Of<Hierarchy>()));
		}

		// The same, for writing. Every call may move the row it returns a
		// pointer into, so a caller holding two of these at once is holding one
		// that has already gone stale.
		Hierarchy *MutableNodeOf(StoreState &state, Entity instance) {
			return static_cast<Hierarchy *>(
				GetComponentMutable(state, instance, Components::Of<Hierarchy>())
			);
		}

		// Copies one instance without its subtree.
		Entity CloneOne(StoreState &state, Entity source) {
			const EntityId key = EntityId::Of(source);
			const EntityLocation from = *state.Directory.Locate(key.Index);
			if (from.Archetype == EntityLocation::NO_ARCHETYPE) {
				return NULL_ENTITY;
			}

			const ComponentSet &set = state.Tables[from.Archetype].Set();

			const Entity copy = CreateEntity(state);
			const EntityId copyKey = EntityId::Of(copy);
			const uint32_t table = TableFor(state, set);
			Relocate(state, copyKey.Index, EntityLocation{}, table);

			// The source row may have moved when the copy was placed, so it is
			// re-read rather than remembered.
			const EntityLocation now = *state.Directory.Locate(key.Index);
			const EntityLocation into = *state.Directory.Locate(copyKey.Index);

			Archetype &destination = state.Tables[into.Archetype];
			Archetype &origin = state.Tables[now.Archetype];

			for (const ComponentId component : set.Ids()) {
				destination.Find(component)->Assign(into.Row, origin.Find(component)->At(now.Row));
			}

			// A copy belongs to no tree until somebody parents it.
			Hierarchy detached;
			destination.Find(Components::Of<Hierarchy>())->Assign(into.Row, &detached);

			return copy;
		}
	}

	Entity CreateInstance(StoreState &state, ClassId id, std::string_view name) {
		const ClassInfo &info = Classes::Describe(id);
		if (info.Set == nullptr) {
			return NULL_ENTITY;
		}

		const Entity entity = CreateEntity(state);
		const EntityId key = EntityId::Of(entity);

		// Straight into the class's archetype rather than one component at a
		// time. Adding them one by one would walk the entity through every
		// intermediate table on the way, creating each of them.
		const uint32_t table = TableFor(state, *info.Set);
		Relocate(state, key.Index, EntityLocation{}, table);

		const EntityLocation location = *state.Directory.Locate(key.Index);
		Archetype &archetype = state.Tables[location.Archetype];

		// The prototype row, copied column by column. This is what makes a
		// default a value rather than a constructor, and it is the same
		// operation Clone performs from a different source.
		for (const ComponentId component : info.Set->Ids()) {
			if (const void *value = Classes::DefaultOf(id, component); value != nullptr) {
				archetype.Find(component)->Assign(location.Row, value);
			}
		}

		const InstanceClass declared{id};
		archetype.Find(Components::Of<InstanceClass>())->Assign(location.Row, &declared);

		if (!name.empty()) {
			const InstanceName label{core::Name(name)};
			archetype.Find(Components::Of<InstanceName>())->Assign(location.Row, &label);
		}

		return entity;
	}

	ClassId ClassOf(const StoreState &state, Entity instance) {
		const auto *declared = static_cast<const InstanceClass *>(
			GetComponent(state, instance, Components::Of<InstanceClass>())
		);
		return declared == nullptr ? ClassId{} : declared->Class;
	}

	bool IsA(const StoreState &state, Entity instance, ClassId id) {
		return Classes::IsA(ClassOf(state, instance), id);
	}

	core::Name InstanceNameOf(const StoreState &state, Entity instance) {
		const auto *label =
			static_cast<const InstanceName *>(GetComponent(state, instance, Components::Of<InstanceName>()));
		return label == nullptr ? core::Name{} : label->Value;
	}

	Entity ParentOf(const StoreState &state, Entity instance) {
		const Hierarchy *node = NodeOf(state, instance);
		return node == nullptr ? NULL_ENTITY : node->Parent;
	}

	void EachChild(const StoreState &state, Entity instance, const std::function<void(Entity)> &body) {
		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return;
		}

		Entity child = node->FirstChild;
		while (child != NULL_ENTITY) {
			// Read before the body runs, so a body that reparents or destroys
			// the child it was handed does not lose its place in the list.
			const Hierarchy *link = NodeOf(state, child);
			const Entity next = link == nullptr ? NULL_ENTITY : link->NextSibling;

			body(child);
			child = next;
		}
	}

	Entity FindFirstChild(const StoreState &state, Entity instance, std::string_view name) {
		const core::Name wanted(name);

		const Hierarchy *node = NodeOf(state, instance);
		if (node == nullptr) {
			return NULL_ENTITY;
		}

		for (Entity child = node->FirstChild; child != NULL_ENTITY;) {
			if (InstanceNameOf(state, child) == wanted) {
				return child;
			}
			const Hierarchy *link = NodeOf(state, child);
			child = link == nullptr ? NULL_ENTITY : link->NextSibling;
		}
		return NULL_ENTITY;
	}

	bool IsDescendantOf(const StoreState &state, Entity instance, Entity ancestor) {
		for (Entity walk = instance; walk != NULL_ENTITY; walk = ParentOf(state, walk)) {
			if (walk == ancestor) {
				return true;
			}
		}
		return false;
	}

	bool SetParent(StoreState &state, Entity instance, Entity parent) {
		Hierarchy *node = MutableNodeOf(state, instance);
		if (node == nullptr) {
			return false;
		}
		if (parent != NULL_ENTITY && MutableNodeOf(state, parent) == nullptr) {
			return false;
		}

		// A cycle is not a wrong answer, it is a hang in every walk of the tree
		// — including the one that would destroy it.
		if (parent != NULL_ENTITY && IsDescendantOf(state, parent, instance)) {
			return false;
		}

		// --- unlink from the old parent ---
		if (node->Parent != NULL_ENTITY) {
			Hierarchy *previous =
				node->PreviousSibling == NULL_ENTITY ? nullptr : MutableNodeOf(state, node->PreviousSibling);
			Hierarchy *next =
				node->NextSibling == NULL_ENTITY ? nullptr : MutableNodeOf(state, node->NextSibling);

			if (previous != nullptr) {
				previous->NextSibling = node->NextSibling;
			}
			if (next != nullptr) {
				next->PreviousSibling = node->PreviousSibling;
			}

			if (Hierarchy *old = MutableNodeOf(state, node->Parent); old != nullptr) {
				if (old->FirstChild == instance) {
					old->FirstChild = node->NextSibling;
				}
				if (old->LastChild == instance) {
					old->LastChild = node->PreviousSibling;
				}
			}

			// Re-read: every lookup above may have moved this row.
			node = MutableNodeOf(state, instance);
		}

		node->Parent = NULL_ENTITY;
		node->NextSibling = NULL_ENTITY;
		node->PreviousSibling = NULL_ENTITY;

		if (parent == NULL_ENTITY) {
			return true;
		}

		// --- link at the end of the new parent, so order is insertion order ---
		Hierarchy *host = MutableNodeOf(state, parent);
		const Entity last = host->LastChild;

		if (last == NULL_ENTITY) {
			host->FirstChild = instance;
			host->LastChild = instance;
		} else {
			MutableNodeOf(state, last)->NextSibling = instance;
			MutableNodeOf(state, parent)->LastChild = instance;
		}

		node = MutableNodeOf(state, instance);
		node->Parent = parent;
		node->PreviousSibling = last;
		return true;
	}

	void DestroyInstance(StoreState &state, Entity instance) {
		if (!IsEntityAlive(state, instance)) {
			return;
		}

		// Children collected before anything is destroyed, because destroying
		// one rewrites the sibling links the walk is standing on.
		std::vector<Entity> children;
		EachChild(state, instance, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			DestroyInstance(state, child);
		}

		SetParent(state, instance, NULL_ENTITY);
		DestroyEntity(state, instance);
	}

	Entity CloneInstance(StoreState &state, Entity source) {
		if (!IsEntityAlive(state, source) || NodeOf(state, source) == nullptr) {
			return NULL_ENTITY;
		}

		const Entity copy = CloneOne(state, source);
		if (copy == NULL_ENTITY) {
			return NULL_ENTITY;
		}

		std::vector<Entity> children;
		EachChild(state, source, [&children](Entity child) { children.push_back(child); });

		for (const Entity child : children) {
			const Entity copied = CloneInstance(state, child);
			if (copied != NULL_ENTITY) {
				SetParent(state, copied, copy);
			}
		}

		return copy;
	}
}
