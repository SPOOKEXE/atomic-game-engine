#include <engine/ecs/Components.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/ImageGraphBinding.hpp>

#include <algorithm>
#include <string_view>

namespace engine::scene {
	namespace {
		bool ValidSelector(core::Name selector) {
			if (!selector.IsValid()) return false;
			const std::string_view text = selector.Text();
			return !text.empty() && text.size() <= IMAGE_GRAPH_BINDING_MAXIMUM_NAME_BYTES &&
				   text.find('\0') == std::string_view::npos;
		}
	}

	bool IsValidImageGraphBinding(const ImageGraphBinding &binding) {
		if (!ValidSelector(binding.Graph) || !ValidSelector(binding.Output) ||
			!ValidSelector(binding.Texture))
			return false;
		if (binding.TickPolicy != ImageGraphTickPolicy::Fixed &&
			binding.TickPolicy != ImageGraphTickPolicy::World)
			return false;
		if (binding.ColorSpace != ImageGraphColorSpace::Display &&
			binding.ColorSpace != ImageGraphColorSpace::Linear)
			return false;
		return std::all_of(binding.Reserved.begin(), binding.Reserved.end(), [](uint8_t byte) {
			return byte == 0;
		});
	}

	bool SetImageGraphBinding(ecs::Store &store, ecs::Entity entity, const ImageGraphBinding &binding) {
		if (!store.Alive(entity) || !IsValidImageGraphBinding(binding) ||
			!ecs::Components::Assigned<ImageGraphBinding>().IsValid())
			return false;

		store.Set(entity, binding);
		return true;
	}
}
