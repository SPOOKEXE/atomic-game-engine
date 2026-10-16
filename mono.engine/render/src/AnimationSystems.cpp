#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Animation.hpp>
#include <engine/scene/Skinning.hpp>

namespace engine::render {
	void RegisterAnimationPresentationSystems(ecs::Scheduler &scheduler) {
		if (!scheduler.HasSystem("evaluate-animations", ecs::Phase::PreRender)) {
			scheduler.Add("evaluate-animations", ecs::Phase::PreRender, [](ecs::Store &store) {
				(void)EvaluateAnimations(store);
			});
		}
		if (!scheduler.HasSystem("resolve-bones", ecs::Phase::PreRender)) {
			scheduler.Add(
				"resolve-bones",
				ecs::Phase::PreRender,
				[](ecs::Store &store) { (void)scene::ResolveBones(store); },
				ecs::SystemOrder{{}, {"evaluate-animations"}}
			);
		}
	}
}
