#include <engine/ecs/Store.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/SurfaceCameras.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

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
		// GLM_FORCE_DEPTH_ZERO_TO_ONE in core's build rather than here - a
		// projection built under a different setting is a depth buffer that is
		// subtly inverted, and that reads as z-fighting rather than as a matrix
		// mistake.
		matrices.View = glm::inverse(frame.ToMatrix());
		matrices.Projection =
			glm::perspective(camera.FieldOfViewRadians, aspectRatio, camera.NearPlane, camera.FarPlane);
		matrices.ViewProjection = matrices.Projection * matrices.View;
		return matrices;
	}

	glm::mat4 ObliqueProjection(
		const glm::mat4 &input, const core::CFrame &frame, const core::Vector3 &normal, float distance
	) {
		glm::mat4 projection = input;

		// No plane means no skew, which is what an unparented surface camera
		// and a degenerate placement both land on.
		const float normalLength = normal.Magnitude();
		if (!(normalLength > 0.0f)) {
			return projection;
		}

		const core::Vector3 unit = normal / normalLength;
		const glm::vec4 world(unit.X, unit.Y, unit.Z, -distance / normalLength);

		// **Into view space by the transpose of the camera's frame, not by its
		// inverse.** A plane is a covector: points go `p_view = View * p_world`
		// with `View = inverse(frame)`, so a plane goes by `inverse(View)`
		// transposed - and `inverse(View)` is the frame matrix itself. Using the
		// view matrix here instead compiles, runs, and clips against a plane
		// that is wrong wherever the camera is rotated.
		const glm::vec4 clip = glm::transpose(frame.ToMatrix()) * world;

		// The camera sits on the plane, so there is no half to keep. Skewing
		// against it produces a matrix with no volume in front of it and the
		// surface renders black.
		constexpr float ON_THE_PLANE = 1.0e-4f;
		if (std::abs(clip.w) < ON_THE_PLANE) {
			return projection;
		}

		// The frustum corner furthest towards the plane, in view space. Scaling
		// the substituted row by this is what keeps the far plane at 1 instead
		// of crushing the usable depth range into whatever the skew left.
		//
		// glm is column-major, so `projection[column][row]`.
		const glm::vec4 corner(
			(std::copysign(1.0f, clip.x) + projection[2][0]) / projection[0][0],
			(std::copysign(1.0f, clip.y) + projection[2][1]) / projection[1][1],
			-1.0f,
			(1.0f + projection[2][2]) / projection[3][2]
		);

		// **`C / (C · Q)` and nothing subtracted**, which is the `0..1` form.
		// See the header: the `-1..1` derivation doubles this and subtracts the
		// `w` row, and using it under `GLM_FORCE_DEPTH_ZERO_TO_ONE` puts the
		// near plane in the wrong place in a way that looks like z-fighting.
		const glm::vec4 substituted = clip * (1.0f / glm::dot(clip, corner));

		projection[0][2] = substituted.x;
		projection[1][2] = substituted.y;
		projection[2][2] = substituted.z;
		projection[3][2] = substituted.w;

		return projection;
	}

	glm::mat4 SurfaceProjection(const SurfaceLens &lens, const core::CFrame &frame) {
		// A degenerate rectangle divides by zero and spreads infinities into
		// every bound derived from the matrix. Identity is the value a consumer
		// can draw nothing from safely, which is the same answer `ResolveCamera`
		// gives a zero aspect ratio.
		if (!(lens.Right > lens.Left) || !(lens.Top > lens.Bottom) || !(lens.NearPlane > 0.0f) ||
			!(lens.FarPlane > lens.NearPlane)) {
			return glm::mat4(1.0f);
		}

		return ObliqueProjection(
			glm::frustum(lens.Left, lens.Right, lens.Bottom, lens.Top, lens.NearPlane, lens.FarPlane),
			frame,
			lens.ClipNormal,
			lens.ClipDistance
		);
	}

	glm::mat4 SeamMatrix(const SeamTransform &through) {
		const glm::mat4 rigid = through.Frame.ToMatrix();
		if (through.Scale == 1.0f) {
			return rigid;
		}

		const glm::vec3 origin{through.Origin.X, through.Origin.Y, through.Origin.Z};
		return rigid * glm::translate(glm::mat4{1.0f}, origin) *
			   glm::scale(glm::mat4{1.0f}, glm::vec3{through.Scale}) *
			   glm::translate(glm::mat4{1.0f}, -origin);
	}

	glm::mat4 SurfaceMapping(const SurfaceLens &lens) {
		const glm::mat4 rigid = lens.Mapping.ToMatrix();

		// **A scale of one is the identity and is worth taking early**, because
		// it is every mirror and every matched pair of panes - which is nearly
		// every surface in nearly every world. Three matrix multiplies avoided
		// per surface per frame, and no float error introduced where there was
		// none.
		if (lens.MappingScale == 1.0f) {
			return rigid;
		}

		// **Taken about the source pane's centre**, which is the point the rigid
		// half already sends to the destination's centre - so scaling here and
		// scaling at the far end are the same map, and only the centre this lens
		// carries is needed to say it.
		const glm::vec3 origin{lens.MappingOrigin.X, lens.MappingOrigin.Y, lens.MappingOrigin.Z};

		glm::mat4 about(1.0f);
		about = glm::translate(about, origin);
		about = glm::scale(about, glm::vec3(lens.MappingScale));
		about = glm::translate(about, -origin);

		return rigid * about;
	}

	CameraMatrices ResolveSurfaceCamera(const core::CFrame &frame, const glm::mat4 &projection) {
		CameraMatrices matrices;
		matrices.View = glm::inverse(frame.ToMatrix());
		matrices.Projection = projection;
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

		// **The near plane the renderer will actually draw with, not the one the
		// scene authored.** A hole is walked up to and then through, and for the
		// last hand's width of that approach an authored near plane slices the
		// pane open - you see through the wall beside the doorway on the one
		// frame the illusion is judged on. `PortalNearPlane` gives the drawing
		// value back without touching the component, so the authored number
		// survives and comes back the moment the eye is clear.
		//
		// **Here as well as in the renderer, and both from the same function.**
		// These matrices are what culling runs against, so a near plane larger
		// than the one the draw uses culls away exactly the geometry that the
		// smaller one exists to keep.
		Camera drawn = *camera;
		drawn.NearPlane =
			PortalNearPlane(camera->NearPlane, NearestSeamDistance(store, transform->Frame.Position));

		const CameraMatrices resolved = ResolveCamera(transform->Frame, drawn, aspectRatio);
		store.ResourceMutable<ActiveCamera>()->Matrices = resolved;
	}
}
