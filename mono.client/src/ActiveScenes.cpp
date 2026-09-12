#include <engine/scene/ActiveCamera.hpp>

#include <algorithm>
#include <client/ActiveScenes.hpp>

namespace client {

	namespace {
		bool Before(
			const ActiveSceneDemand &left, const ActiveSceneDemand &right, engine::world::Universe &universe
		) {
			const engine::core::Name leftName = universe.NameOf(left.Request.World);
			const engine::core::Name rightName = universe.NameOf(right.Request.World);
			if (leftName != rightName) return leftName.Text() < rightName.Text();
			return left.Request.World.Index < right.Request.World.Index;
		}
	}

	size_t ActiveSceneCollector::Collect(
		engine::world::Universe &universe,
		std::span<const ActiveSceneDemand> requests,
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
				[](const auto &left, const auto &right) { return left.Request.World == right.Request.World; }
			),
			Demands.end()
		);

		Collected.resize(Demands.size());
		for (size_t index = 0; index < Demands.size(); index++) {
			ActiveScene &scene = Collected[index];
			scene.World = Demands[index].Request.World;
			scene.Name = universe.NameOf(scene.World);
			scene.Pipeline = Demands[index].Pipeline;
			scene.Frame = std::make_unique<engine::render::WorldViewFrame>();
			scene.CameraLayers = std::make_unique<engine::render::WorldCameraFrame>();
		}
		std::vector<engine::world::Presentation> presentations;
		presentations.reserve(Demands.size());
		for (const ActiveSceneDemand &demand : Demands)
			presentations.push_back(demand.Request);
		(void)universe.PresentMany(
			presentations, [&](engine::world::WorldId world, engine::ecs::Store &store) {
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
						 .Pipeline = scene.Pipeline},
						scene.View
					))
					return;
				scene.Camera = active->Entity;
			}
		);
		std::erase_if(Collected, [](const auto &scene) { return scene.Camera == engine::ecs::NULL_ENTITY; });

		Submitted.reserve(Collected.size());
		for (const auto &scene : Collected)
			Submitted.push_back(scene.View);
		return Submitted.size();
	}

	engine::render::FrameResult ActiveSceneCollector::SubmitBatch(
		engine::world::WorldId displayedWorld,
		const engine::render::View &displayedView,
		uint32_t width,
		uint32_t height,
		bool offscreenDisplayed,
		std::span<const engine::render::WorldContentOwner> foreignContentOwners,
		const std::function<engine::render::FrameResult(std::span<const engine::render::View>)> &submit
	) {
		BatchTargets.clear();
		BatchViews.clear();
		BatchTargets.reserve(Collected.size());
		BatchViews.reserve(Collected.size() + 1);
		for (const ActiveScene &scene : Collected) {
			if (scene.World == displayedWorld) continue;
			BatchTargets.push_back({std::max(width, 1u), std::max(height, 1u)});
			engine::render::View view = scene.View;
			view.Target = &BatchTargets.back();
			view.Slot = BatchViews.size() + 1;
			view.ForeignContentOwners = foreignContentOwners;
			BatchViews.push_back(view);
		}
		engine::render::View finalView = displayedView;
		if (offscreenDisplayed) {
			BatchTargets.push_back({std::max(width, 1u), std::max(height, 1u)});
			finalView.Target = &BatchTargets.back();
		}
		BatchViews.push_back(finalView);
		if (!submit) return {};
		return submit(BatchViews);
	}
}
