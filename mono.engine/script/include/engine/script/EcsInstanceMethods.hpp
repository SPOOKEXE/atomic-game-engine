#pragma once

// The ECS methods installed on every script Instance.
//
// The method implementations belong to their VM adapters, but their public
// names are shared runtime data so tooling and both adapters use one catalogue.

#include <cstdint>
#include <span>

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
}
