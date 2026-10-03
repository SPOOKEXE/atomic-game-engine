#pragma once
#include <engine/imagegraph/GroupReplay.hpp>

#include <algorithm>
namespace engine::imagegraph::detail {
	inline std::string_view BindingAnimatorPort(const GroupSubtypeBinding &binding) noexcept {
		return binding.AnimatorPort.empty() ? std::string_view(binding.Port)
											: std::string_view(binding.AnimatorPort);
	}
	inline bool MovedSourceAnimator(const GroupSubtypeBinding &binding) noexcept {
		return !binding.AnimatorPort.empty() && binding.AnimatorPort != binding.Port;
	}
	inline bool InheritedMovedSourceGetter(
		const Node &node, std::string_view port, const GroupSubtypeBinding *binding
	) noexcept {
		return binding && MovedSourceAnimator(*binding) &&
			   std::find(node.InstanceOverrides.begin(), node.InstanceOverrides.end(), port) ==
				   node.InstanceOverrides.end();
	}
	inline std::string_view
	SourceGetterPort(const Node &node, std::string_view port, const GroupSubtypeBinding *binding) noexcept {
		return binding && !InheritedMovedSourceGetter(node, port, binding) ? BindingAnimatorPort(*binding)
																		   : port;
	}
}
