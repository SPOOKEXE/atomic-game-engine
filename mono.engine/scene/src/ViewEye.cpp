#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/ViewEye.hpp>

namespace engine::scene {
	bool ResolveViewEye(const ecs::Store &store, ecs::Entity camera, core::CFrame &frame) {
		if (!store.Alive(camera)) return false;
		const auto *transform = store.Get<Transform>(camera);
		if (transform == nullptr) return false;
		const auto *view = store.Resource<ViewEye>();
		frame = view != nullptr && view->Camera == camera ? view->Frame : transform->Frame;
		return true;
	}
}
