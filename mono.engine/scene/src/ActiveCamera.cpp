#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <glm/gtc/matrix_transform.hpp>

namespace engine::scene {

	CameraMatrices ResolveCamera(const core::CFrame &frame, const Camera &camera, float aspectRatio) {
		CameraMatrices matrices;

		// A minimised window reports zero height, and a mirror target can be
		// configured before it has a size. `glm::perspective` divides by the
		// aspect, so both of those produce a matrix of infinities that then
		// spreads into every culled bound and every interpolated position.
		// Identity is the value a consumer can draw nothing from safely.
		if (!(aspectRatio > 0.0f)) {
			return matrices;
		}

		// No Y flip, matching what the renderer submits: SDL's Vulkan backend
		// already applies a negative-height viewport, so clip space is Y-up on
		// every backend. Depth is 0..1, pinned engine-wide by
		// GLM_FORCE_DEPTH_ZERO_TO_ONE in core's build rather than here — a
		// projection built under a different setting is a depth buffer that is
		// subtly inverted, and that reads as z-fighting rather than as a matrix
		// mistake.
		matrices.View = glm::inverse(frame.ToMatrix());
		matrices.Projection =
			glm::perspective(camera.FieldOfViewRadians, aspectRatio, camera.NearPlane, camera.FarPlane);
		matrices.ViewProjection = matrices.Projection * matrices.View;
		return matrices;
	}

	void ResolveActiveCamera(ecs::Store &store) {
		const ActiveCamera *active = store.Resource<ActiveCamera>();
		if (active == nullptr) {
			return;
		}

		// Read out of the resource before anything else is touched: the
		// resolved matrices are written back through a second lookup, and
		// holding a pointer across the component reads would be holding a
		// pointer into storage those reads may move.
		const ecs::Entity entity = active->Entity;
		const float aspectRatio = active->AspectRatio;

		const Camera *camera = store.Get<Camera>(entity);
		const Transform *transform = store.Get<Transform>(entity);
		if (camera == nullptr || transform == nullptr) {
			return;
		}

		const CameraMatrices resolved = ResolveCamera(transform->Frame, *camera, aspectRatio);
		store.ResourceMutable<ActiveCamera>()->Matrices = resolved;
	}
}
