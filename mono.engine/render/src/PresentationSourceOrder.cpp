#include "PresentationSourceOrder.hpp"

#include "PresentationSource.hpp"

#include <engine/ecs/Store.hpp>

#include <algorithm>

namespace engine::render {
	void FinalizePresentationSourceRows(ecs::Store &store, DrawList &drawList, bool removeFullyTransparent) {
		for (scene::DrawInstance &instance : drawList.Instances) {
			PresentationSource::ApplyOptionalRenderState(store, ecs::Entity(instance.Source), instance);
		}
		if (removeFullyTransparent) {
			std::erase_if(drawList.Instances, [](const scene::DrawInstance &instance) {
				return instance.Transparency >= 1.0f;
			});
		}
	}

	void AppendPresentationSeamRows(ecs::Store &store, DrawList &drawList) {
		(void)scene::CutAndCloneSeams(store, drawList.Instances);
	}
}
