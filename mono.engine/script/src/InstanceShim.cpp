#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/script/InstanceShim.hpp>

namespace engine::script {

	std::vector<const ecs::PropertyDescriptor *>
	ScriptableProperties(const ecs::Store &store, ecs::Entity instance) {
		std::vector<const ecs::PropertyDescriptor *> properties;
		for (const ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Scriptable) {
				properties.push_back(&property);
			}
		}
		return properties;
	}

	const ecs::PropertyDescriptor *
	ScriptableProperty(const ecs::Store &store, ecs::Entity instance, std::string_view name) {
		for (const ecs::PropertyDescriptor &property : store.PropertiesOf(instance)) {
			if (property.Spelling == name) {
				return property.Scriptable ? &property : nullptr;
			}
		}
		return nullptr;
	}

	bool ReadInstanceProperty(
		const ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &property,
		void *value,
		size_t bytes
	) {
		return property.Scriptable && store.GetProperty(instance, property, value, bytes);
	}

	bool WriteInstanceProperty(
		ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &property,
		const void *value,
		size_t bytes
	) {
		return property.Scriptable && property.Writable &&
			   store.SetProperty(instance, property, value, bytes);
	}

	ecs::Entity FindInstanceChild(const ecs::Store &store, ecs::Entity instance, std::string_view name) {
		return store.FindFirstChild(instance, name);
	}

	bool InstanceAlive(const ecs::Store &store, ecs::Entity instance) {
		return store.Alive(instance);
	}

	ecs::ClassId InstanceClassOf(const ecs::Store &store, ecs::Entity instance) {
		return store.ClassOf(instance);
	}

	bool InstanceIsA(const ecs::Store &store, ecs::Entity instance, ecs::ClassId wanted) {
		return store.IsA(instance, wanted);
	}

	core::Name InstanceNameOf(const ecs::Store &store, ecs::Entity instance) {
		return store.InstanceNameOf(instance);
	}

	ecs::Entity InstanceParentOf(const ecs::Store &store, ecs::Entity instance) {
		return store.ParentOf(instance);
	}

	InstanceCreateResult
	CreateScriptInstance(ecs::Store &store, std::string_view className, ecs::Entity parent) {
		const ecs::ClassId id = ecs::Classes::Find(core::Name(className));
		if (!id.IsValid()) {
			return {.Failure = InstanceCreateFailure::UnknownClass};
		}

		const ecs::Entity instance = store.CreateInstance(id, className);
		if (instance == ecs::NULL_ENTITY) {
			return {.Failure = InstanceCreateFailure::StoreRefused};
		}

		if (parent != ecs::NULL_ENTITY && !store.SetParent(instance, parent)) {
			store.DestroyInstance(instance);
			return {.Failure = InstanceCreateFailure::ParentRefused};
		}
		return {.Instance = instance};
	}
}
