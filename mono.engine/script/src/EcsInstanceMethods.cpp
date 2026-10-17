// The public names for adapter-implemented ECS Instance methods.

#include <engine/script/EcsInstanceMethods.hpp>

#include <array>

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
}
