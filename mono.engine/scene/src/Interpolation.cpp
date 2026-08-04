#include <engine/scene/Components.hpp>
#include <engine/scene/Interpolation.hpp>

namespace engine::scene {

	void CapturePreviousTransforms(ecs::Store &store) {
		store.EachParallel<PreviousTransform, const Transform>(
			[](ecs::Entity, PreviousTransform &previous, const Transform &transform) {
				previous.Frame = transform.Frame;
			}
		);
	}
}
