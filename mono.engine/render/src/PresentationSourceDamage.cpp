#include "PresentationSourceDamage.hpp"

#include "PresentationSourceDamageTable.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Visibility.hpp>

#include <tuple>

namespace engine::render {

	PresentationSourceDamage CollectPresentationSourceDamage(
		ecs::Store &store, DrawList &drawList, size_t matching, size_t skeletons, size_t bones
	) {
		PresentationSourceDamage damage;
		damage.Full = !drawList.SourcesReady || matching != drawList.SourceEntityCount ||
					  skeletons != drawList.SkeletonCount || bones != drawList.BoneCount;
		const auto drawable = [&store](ecs::Entity entity) {
			return PresentationSource::IsWorldDrawable(store, entity);
		};
		std::apply(
			[&](const auto &...source) { (source.Collect(store, drawList, drawable, damage), ...); },
			presentation_source_damage_detail::SOURCE_TABLE
		);
		drawList.SourceEntityCount = matching;
		drawList.SkeletonCount = skeletons;
		drawList.BoneCount = bones;
		drawList.SourcesReady = true;
		return damage;
	}
}
