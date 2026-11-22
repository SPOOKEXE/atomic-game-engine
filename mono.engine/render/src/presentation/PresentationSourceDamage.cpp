#include "presentation/PresentationSourceDamage.hpp"

#include "presentation/PresentationSourceDamageTable.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/cameras/Visibility.hpp>
#include <engine/scene/characters/Animation.hpp>
#include <engine/scene/characters/Skinning.hpp>

#include <algorithm>
#include <tuple>

namespace engine::render {

	PresentationSourceDamage CollectPresentationSourceDamage(
		ecs::Store &store, DrawList &drawList, size_t matching, size_t skeletons, size_t bones
	) {
		PresentationSourceDamage damage;
		damage.Full = !drawList.SourcesReady || matching != drawList.SourceEntityCount ||
					  skeletons != drawList.SkeletonCount || bones != drawList.BoneCount;
		const auto drawable = [&store, &drawList](ecs::Entity entity) {
			if (drawList.DrawableSourcesReady) {
				return std::binary_search(
					drawList.DrawableSourceIds.begin(), drawList.DrawableSourceIds.end(), entity.Id
				);
			}
			return PresentationSource::IsWorldDrawable(store, entity);
		};
		std::apply(
			[&](const auto &...source) { (source.Collect(store, drawList, drawable, damage), ...); },
			presentation_source_damage_detail::SOURCE_TABLE
		);
		// A previewed pose moves no row, so it is pose damage while a preview is
		// installed and on the frame after one, which draws the authored pose again.
		const auto *preview = store.Resource<scene::AnimationPreview>();
		const bool previewed = preview != nullptr && !preview->WorldFrames.empty();
		damage.Pose = damage.Pose || previewed || drawList.PreviewPosed;
		drawList.PreviewPosed = previewed;
		drawList.SourceEntityCount = matching;
		drawList.SkeletonCount = skeletons;
		drawList.BoneCount = bones;
		drawList.SourcesReady = true;
		if (damage.Full) drawList.DrawableSourcesReady = false;
		return damage;
	}
}
