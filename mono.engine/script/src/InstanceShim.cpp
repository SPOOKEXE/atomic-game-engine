#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/scene/Services.hpp>
#include <engine/script/InstanceShim.hpp>

namespace engine::script {

	bool InstanceVisibleToScript(const ecs::Store &store, ecs::Entity instance, bool clientExecution) {
		if (!store.Alive(instance)) return false;
		return clientExecution || store.AdoptOnly()
				   ? !gui::IsPlayerGuiSource(store, instance)
				   : !ecs::IsClientLocalInstance(store, instance) && !gui::IsPlayerGuiCopy(store, instance);
	}

	ecs::Entity InstanceForScriptRead(const ecs::Store &store, ecs::Entity instance, bool clientExecution) {
		if (instance == ecs::NULL_ENTITY) return instance;
		if ((clientExecution || store.AdoptOnly()) &&
			(!store.Alive(instance) || gui::IsPlayerGuiSource(store, instance))) {
			return gui::FindPlayerGuiCopy(store, instance);
		}
		return InstanceVisibleToScript(store, instance, clientExecution) ? instance : ecs::NULL_ENTITY;
	}

	void EachInstanceChild(
		const ecs::Store &store,
		ecs::Entity instance,
		const std::function<void(ecs::Entity)> &body,
		bool clientExecution
	) {
		instance = InstanceForScriptRead(store, instance, clientExecution);
		if (instance == ecs::NULL_ENTITY) return;
		store.EachChild(instance, [&](ecs::Entity child) {
			if (InstanceVisibleToScript(store, child, clientExecution)) body(child);
		});
	}

	void EachInstanceDescendant(
		const ecs::Store &store,
		ecs::Entity instance,
		const std::function<void(ecs::Entity)> &body,
		bool clientExecution
	) {
		instance = InstanceForScriptRead(store, instance, clientExecution);
		if (instance == ecs::NULL_ENTITY) return;
		store.EachDescendant(instance, [&](ecs::Entity child) {
			if (InstanceVisibleToScript(store, child, clientExecution)) body(child);
		});
	}

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
		size_t bytes,
		bool clientExecution
	) {
		instance = InstanceForScriptRead(store, instance, clientExecution);
		if (instance == ecs::NULL_ENTITY || !property.Scriptable ||
			!store.GetProperty(instance, property, value, bytes))
			return false;
		if (property.Type == ecs::PropertyType::Reference && bytes == sizeof(ecs::Entity)) {
			auto &reference = *static_cast<ecs::Entity *>(value);
			reference = InstanceForScriptRead(store, reference, clientExecution);
		}
		return true;
	}

	bool WriteInstanceProperty(
		ecs::Store &store,
		ecs::Entity instance,
		const ecs::PropertyDescriptor &property,
		const void *value,
		size_t bytes,
		bool clientExecution
	) {
		if (!property.Scriptable || !property.Writable) return false;
		const bool client = clientExecution || store.AdoptOnly();
		if (!client && ecs::IsClientLocalInstance(store, instance)) return false;
		if (!client && property.Type == ecs::PropertyType::Reference && value != nullptr &&
			bytes == sizeof(ecs::Entity) &&
			ecs::IsClientLocalInstance(store, *static_cast<const ecs::Entity *>(value)))
			return false;
		if (client && property.Kind == ecs::PropertyKind::Resource && !property.PredictedWritable)
			return false;
		const bool owned = ecs::Store::IsPredicted(instance) && store.Alive(instance) &&
						   ecs::IsClientLocalInstance(store, instance);
		if ((clientExecution || store.AdoptOnly()) && !owned &&
			!(property.PredictedWritable && ecs::Store::IsPredicted(instance)))
			return false;
		if (!owned) return store.SetProperty(instance, property, value, bytes);
		return store.SetPropertyAuthored(instance, property, value, bytes);
	}

	ecs::Entity FindInstanceChild(
		const ecs::Store &store,
		ecs::Entity instance,
		std::string_view name,
		bool recursive,
		bool clientExecution
	) {
		if (name.empty()) return ecs::NULL_ENTITY;
		const core::Name wanted(name);
		ecs::Entity found;
		const auto match = [&](ecs::Entity child) {
			if (found == ecs::NULL_ENTITY && store.InstanceNameOf(child) == wanted) found = child;
		};
		EachInstanceChild(store, instance, match, clientExecution);
		if (found == ecs::NULL_ENTITY && recursive)
			EachInstanceDescendant(store, instance, match, clientExecution);
		return found;
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

	InstanceCreateResult CreateScriptInstance(
		ecs::Store &store, std::string_view className, ecs::Entity parent, bool clientExecution
	) {
		const ecs::ClassId id = ecs::Classes::Find(core::Name(className));
		if (!id.IsValid()) {
			return {.Failure = InstanceCreateFailure::UnknownClass};
		}
		if (!ecs::Classes::Describe(id).Creatable) {
			return {.Failure = InstanceCreateFailure::NotCreatable};
		}

		if (!clientExecution && !store.AdoptOnly() &&
			(ecs::Classes::Describe(id).RuntimeLocal || ecs::IsClientLocalInstance(store, parent)))
			return {.Failure = InstanceCreateFailure::StoreRefused};

		const bool local = clientExecution || store.AdoptOnly() || ecs::Classes::Describe(id).RuntimeLocal ||
						   ecs::IsClientLocalInstance(store, parent);
		const ecs::Entity instance =
			local ? store.CreatePredictedInstance(id, className) : store.CreateInstance(id, className);
		if (instance == ecs::NULL_ENTITY) {
			return {.Failure = InstanceCreateFailure::StoreRefused};
		}

		if (local) store.Set(instance, ecs::ClientLocal{});

		if (parent != ecs::NULL_ENTITY && !store.SetParent(instance, parent)) {
			store.DestroyInstance(instance);
			return {.Failure = InstanceCreateFailure::ParentRefused};
		}
		return {.Instance = instance};
	}
	ecs::Entity CloneScriptInstance(ecs::Store &store, ecs::Entity source, bool clientExecution) {
		if (!clientExecution && !store.AdoptOnly() && ecs::IsClientLocalInstance(store, source))
			return ecs::NULL_ENTITY;
		const bool local = clientExecution || store.AdoptOnly() || ecs::IsClientLocalInstance(store, source);
		const ecs::Entity copy = local ? store.ClonePredictedInstance(source) : store.CloneInstance(source);
		if (local && copy != ecs::NULL_ENTITY) {
			std::vector<ecs::Entity> pending{copy};
			while (!pending.empty()) {
				const ecs::Entity instance = pending.back();
				pending.pop_back();
				store.Set(instance, ecs::ClientLocal{});
				store.EachChild(instance, [&](ecs::Entity child) { pending.push_back(child); });
			}
		}
		return copy;
	}
}
