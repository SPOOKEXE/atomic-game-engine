#pragma once

// The ECS methods installed on every script Instance.
//
// The method implementations belong to their VM adapters, but their public
// names are shared runtime data so tooling and both adapters use one catalogue.

#include <cstdint>
#include <span>

namespace engine::script {

	enum class EcsInstanceMethod : uint8_t {
		SetComponent,
		GetComponent,
		HasComponent,
		RemoveComponent,
		GetComponents,
	};

	struct EcsInstanceMethodDescriptor {
		EcsInstanceMethod Method;
		const char *Name;
	};

	// The shared names for ECS methods that both VM adapters install on Instance.
	std::span<const EcsInstanceMethodDescriptor> EcsInstanceMethods();

	// The script-visible name for one ECS Instance method.
	const char *EcsInstanceMethodName(EcsInstanceMethod method);
}
