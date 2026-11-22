#pragma once

// The ECS methods installed on every script Instance.
//
// The method implementations belong to their VM adapters, but their public
// names are shared runtime data so tooling and both adapters use one catalogue.

#include <engine/ecs/ComponentId.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/StoreFwd.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace engine::ecs {
	struct PropertyDescriptor;
}

namespace engine::script {

	// Script-visible ECS methods installed on every Instance.
	enum class EcsInstanceMethod : uint8_t {
		SetComponent,
		GetComponent,
		HasComponent,
		RemoveComponent,
		GetComponents,
	};

	// Script method enum paired with its public name.
	struct EcsInstanceMethodDescriptor {
		// Method implemented by the VM adapter.
		EcsInstanceMethod Method;
		// Script-visible method name.
		const char *Name;
	};

	// The shared names for ECS methods that both VM adapters install on Instance.
	std::span<const EcsInstanceMethodDescriptor> EcsInstanceMethods();

	// The script-visible name for one ECS Instance method.
	const char *EcsInstanceMethodName(EcsInstanceMethod method);

	// The scriptable properties that project one engine component on an entity.
	//
	// An engine component is a C++ struct with no field list, so a script reads
	// and writes it through the properties already declared over it: every
	// scriptable property whose `Reads` set holds `component`. That keeps one
	// write path per component. An instance answers from its own class; a
	// classless entity answers from every class, keeping the first property of
	// each spelling whose reads the entity carries in full.
	std::vector<const ecs::PropertyDescriptor *>
	EngineComponentProperties(const ecs::Store &store, ecs::Entity entity, ecs::ComponentId component);

	// Whether a script may add or remove this engine component.
	//
	// False for the components that make an entity an instance (class, tree,
	// name, projection, client ownership). Those change only through the
	// instance surface, because a raw write would break the tree's invariants.
	bool EngineComponentScriptWritable(ecs::ComponentId component);

	// Adds a default-constructed `component` when `entity` does not carry it.
	//
	// @return `false` when the entity is gone or the store refused the row.
	bool AddDefaultComponent(ecs::Store &store, ecs::Entity entity, ecs::ComponentId component);

	// Whether a method or host call of this name may run inside a read-only
	// pass such as `World:ForEachParallel`.
	//
	// Decided by spelling: a name whose last dotted segment starts with `Get`,
	// `Find`, `Has`, `Is`, `Count` or `Equals` reads. Anything else may write,
	// so it is refused. A refused read is a script error the author can see; an
	// allowed write would be a parallel body changing the world under its
	// neighbours.
	bool ReadOnlyMethodName(std::string_view name);
}
