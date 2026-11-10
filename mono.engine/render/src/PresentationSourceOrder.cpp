#include "PresentationSourceOrder.hpp"

#include "PresentationSource.hpp"

#include <engine/ecs/Store.hpp>

#include <algorithm>

namespace engine::render {
	void FinalizePresentationSourceRows(ecs::Store &store, DrawList &drawList, bool removeFullyTransparent) {
		// Four lookups a row for components most worlds never add.
		if (PresentationSource::AnyRenderState(store)) {
			for (scene::DrawInstance &instance : drawList.Instances) {
				PresentationSource::ApplyOptionalRenderState(store, ecs::Entity(instance.Source), instance);
			}
		}
		// A rebuild that walked the same rows in the same order keeps its sorted set.
		const bool sameOrder =
			drawList.DrawableSourceOrder.size() == drawList.Instances.size() &&
			std::equal(
				drawList.Instances.begin(),
				drawList.Instances.end(),
				drawList.DrawableSourceOrder.begin(),
				[](const scene::DrawInstance &instance, uint64_t id) { return instance.Source == id; }
			);
		if (!sameOrder) {
			drawList.DrawableSourceOrder.clear();
			drawList.DrawableSourceOrder.reserve(drawList.Instances.size());
			for (const scene::DrawInstance &instance : drawList.Instances) {
				drawList.DrawableSourceOrder.push_back(instance.Source);
			}
			drawList.DrawableSourceIds.assign(
				drawList.DrawableSourceOrder.begin(), drawList.DrawableSourceOrder.end()
			);
			std::sort(drawList.DrawableSourceIds.begin(), drawList.DrawableSourceIds.end());
			drawList.DrawableSourceIds.erase(
				std::unique(drawList.DrawableSourceIds.begin(), drawList.DrawableSourceIds.end()),
				drawList.DrawableSourceIds.end()
			);
		}
		drawList.DrawableSourcesReady = true;
		if (removeFullyTransparent) {
			std::erase_if(drawList.Instances, [](const scene::DrawInstance &instance) {
				return instance.Transparency >= 1.0f;
			});
		}
	}

	void AppendPresentationSeamRows(ecs::Store &store, DrawList &drawList) {
		// Source rows survive between collections; the cut from the previous
		// presentation does not. Recompute it even when the last mouth vanished.
		for (scene::DrawInstance &instance : drawList.Instances) {
			if (instance.SeamMask == 0) continue;
			instance.SeamNormal = {};
			instance.SeamOffset = 0.0f;
			instance.SeamFirst = {};
			instance.SeamSecond = {};
			instance.SeamCentre = {};
			instance.SeamMask = 0;
		}
		(void)scene::CutAndCloneSeams(store, drawList.Instances);
	}
}
