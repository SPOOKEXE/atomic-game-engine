#include <engine/scene/ActiveCamera.hpp>

#include <algorithm>
#include <client/ActiveScenes.hpp>

namespace client {

	namespace {
		bool Before(
			const engine::world::Presentation &left,
			const engine::world::Presentation &right,
			engine::world::Universe &universe
		) {
			const engine::core::Name leftName = universe.NameOf(left.World);
			const engine::core::Name rightName = universe.NameOf(right.World);
			if (leftName != rightName) return leftName.Text() < rightName.Text();
			return left.World.Index < right.World.Index;
		}
	}

	size_t ActiveSceneCollector::Collect(
		engine::world::Universe &universe,
		std::span<const engine::world::Presentation> requests,
		const engine::core::Vector2 &extent
	) {
		Demands.assign(requests.begin(), requests.end());
		Collected.clear();
		Submitted.clear();
		std::sort(Demands.begin(), Demands.end(), [&](const auto &left, const auto &right) {
			return Before(left, right, universe);
		});
		Demands.erase(
			std::unique(
				Demands.begin(),
				Demands.end(),
				[](const auto &left, const auto &right) { return left.World == right.World; }
			),
			Demands.end()
		);

		Collected.resize(Demands.size());
		for (size_t index = 0; index < Demands.size(); index++) {
			ActiveScene &scene = Collected[index];
			scene.World = Demands[index].World;
			scene.Name = universe.NameOf(scene.World);
			scene.Frame = std::make_unique<engine::render::WorldViewFrame>();
			scene.CameraLayers = std::make_unique<engine::render::WorldCameraFrame>();
		}
		(void)universe.PresentMany(Demands, [&](engine::world::WorldId world, engine::ecs::Store &store) {
			auto found = std::find_if(Collected.begin(), Collected.end(), [world](const auto &scene) {
				return scene.World == world;
			});
			if (found == Collected.end() || !found->Name.IsValid()) return;
			ActiveScene &scene = *found;
			const auto *active = store.Resource<engine::scene::ActiveCamera>();
			if (active == nullptr || !store.Alive(active->Entity)) return;
			const auto *placement = store.Get<engine::scene::Transform>(active->Entity);
			const auto *lens = store.Get<engine::scene::Camera>(active->Entity);
			if (placement == nullptr || lens == nullptr) return;

			engine::render::CollectWorldView(store, scene.Name, *scene.Frame);
			scene.View.CameraFrame = placement->Frame;
			scene.View.Camera = *lens;
			scene.View.Portals = scene.Frame->Portals;
			engine::render::CollectWorldCamera(store, scene.View, extent, *scene.CameraLayers);
			if (!engine::render::BindWorldView(
					*scene.Frame,
					*scene.CameraLayers,
					{.World = world.Index,
					 .Name = scene.Name,
					 .Identity = store.Identity(),
					 .ContentOwner = scene.Name,
					 .ForeignContentOwners = {},
					 .Pipeline = {}},
					scene.View
				))
				return;
			scene.Camera = active->Entity;
		});
		std::erase_if(Collected, [](const auto &scene) { return scene.Camera == engine::ecs::NULL_ENTITY; });

		Submitted.reserve(Collected.size());
		for (const auto &scene : Collected)
			Submitted.push_back(scene.View);
		return Submitted.size();
	}
}
