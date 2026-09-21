#include "DataFactoryPausedPresentation.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <client/Compositor.hpp>

namespace client {

	std::optional<PausedDataFactoryPresentation> PublishPausedDataFactoryPresentation(
		engine::ecs::Store &store, Compositor &views, engine::world::WorldId world
	) {
		engine::render::CollectInstances(store, engine::render::DrawCollectionTime::CurrentTick);
		const auto *draw = store.Resource<engine::render::DrawList>();
		const auto *active = store.Resource<engine::scene::ActiveCamera>();
		if (draw == nullptr || active == nullptr || !store.Alive(active->Entity)) return std::nullopt;
		const auto *placement = store.Get<engine::scene::Transform>(active->Entity);
		const auto *camera = store.Get<engine::scene::Camera>(active->Entity);
		if (placement == nullptr || camera == nullptr) return std::nullopt;
		if (!views.Publish(
				world, placement->Frame, *camera, draw->Instances, store.Time().Tick, 1.0f, draw->JointFrames
			))
			return std::nullopt;
		return PausedDataFactoryPresentation{
			.Frame = placement->Frame,
			.Camera = *camera,
			.Tick = store.Time().Tick,
			.ObjectLabels = draw->ObjectLabels,
			.SemanticLabels = draw->SemanticLabels,
			.PartLabels = draw->PartLabels,
			.ObjectLabelsValid = draw->ObjectLabelsValid,
			.SemanticLabelsValid = draw->SemanticLabelsValid,
			.PartLabelsValid = draw->PartLabelsValid,
		};
	}
}
