// The public names for adapter-implemented ECS Instance methods.

#include <engine/ecs/Classes.hpp>
#include <engine/ecs/ComponentSet.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/script/EcsInstanceMethods.hpp>

#include <array>
#include <new>

namespace engine::script {

	namespace {
		constexpr std::array<EcsInstanceMethodDescriptor, 5> METHODS{{
			{EcsInstanceMethod::SetComponent, "SetComponent"},
			{EcsInstanceMethod::GetComponent, "GetComponent"},
			{EcsInstanceMethod::HasComponent, "HasComponent"},
			{EcsInstanceMethod::RemoveComponent, "RemoveComponent"},
			{EcsInstanceMethod::GetComponents, "GetComponents"},
		}};
	}

	std::span<const EcsInstanceMethodDescriptor> EcsInstanceMethods() {
		return METHODS;
	}

	const char *EcsInstanceMethodName(EcsInstanceMethod method) {
		for (const EcsInstanceMethodDescriptor &descriptor : METHODS) {
			if (descriptor.Method == method) {
				return descriptor.Name;
			}
		}
		return "";
	}

	std::vector<const ecs::PropertyDescriptor *>
	EngineComponentProperties(const ecs::Store &store, ecs::Entity entity, ecs::ComponentId component) {
		std::vector<const ecs::PropertyDescriptor *> found;
		const auto projects = [component](const ecs::PropertyDescriptor &property) {
			return property.Scriptable && property.Reads != nullptr && property.Reads->Contains(component);
		};

		if (store.ClassOf(entity).IsValid()) {
			for (const ecs::PropertyDescriptor &property : store.PropertiesOf(entity)) {
				if (projects(property)) found.push_back(&property);
			}
			return found;
		}

		const auto carried = [&store, entity](const ecs::PropertyDescriptor &property) {
			for (const ecs::ComponentId read : property.Reads->Ids()) {
				if (!store.HasComponent(entity, read)) return false;
			}
			return true;
		};
		const auto seen = [&found](std::string_view spelling) {
			for (const ecs::PropertyDescriptor *property : found) {
				if (property->Spelling == spelling) return true;
			}
			return false;
		};
		for (size_t index = 0; index < ecs::Classes::Count(); index++) {
			const ecs::ClassInfo &info = ecs::Classes::Describe(ecs::ClassId(static_cast<uint32_t>(index)));
			for (const ecs::PropertyDescriptor &property : info.Properties) {
				if (projects(property) && carried(property) && !seen(property.Spelling)) {
					found.push_back(&property);
				}
			}
		}
		return found;
	}

	bool EngineComponentScriptWritable(ecs::ComponentId component) {
		using ecs::Components;
		return component.IsValid() && component != Components::Of<ecs::Hierarchy>() &&
			   component != Components::Of<ecs::InstanceClass>() &&
			   component != Components::Of<ecs::InstanceName>() &&
			   component != Components::Of<ecs::InstanceProjection>() &&
			   component != Components::Of<ecs::ClientLocal>();
	}

	bool AddDefaultComponent(ecs::Store &store, ecs::Entity entity, ecs::ComponentId component) {
		if (!store.Alive(entity)) return false;
		if (store.HasComponent(entity, component)) return true;

		const ecs::TypeDescriptor &type = ecs::Components::Describe(component);
		if (type.Size == 0) {
			store.SetComponent(entity, component, nullptr);
			return store.HasComponent(entity, component);
		}
		void *bytes = ::operator new(type.Size, std::align_val_t(type.Alignment));
		type.DefaultConstruct(bytes, 1);
		store.SetComponent(entity, component, bytes);
		type.Destruct(bytes, 1);
		::operator delete(bytes, std::align_val_t(type.Alignment));
		return store.HasComponent(entity, component);
	}

	bool ReadOnlyMethodName(std::string_view name) {
		const size_t dot = name.find_last_of(".:");
		if (dot != std::string_view::npos) name = name.substr(dot + 1);
		constexpr std::array<std::string_view, 6> READS{"Get", "Find", "Has", "Is", "Count", "Equals"};
		for (const std::string_view prefix : READS) {
			if (name.starts_with(prefix)) return true;
		}
		return false;
	}
}
