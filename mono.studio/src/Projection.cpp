#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec4.hpp>

#include <cmath>
#include <studio/Projection.hpp>

namespace studio {

	using engine::core::CFrame;
	using engine::core::Ray;
	using engine::core::Vector3;

	namespace {
		// Anything at or behind this much clip-space `w` is treated as behind
		// the camera.
		//
		// **Not zero.** The perspective divide by a `w` approaching zero sends a
		// point to infinity, and a line clipped at exactly the eye plane still
		// produces coordinates in the millions - which imgui then rasterises as
		// a stripe across the whole panel. A small positive epsilon is the
		// difference between "clipped" and "clipped, and the last segment ate
		// the screen".
		constexpr float NEAR_W = 1e-4f;
	}

	bool IntersectRayPlane(Vector3 origin, Vector3 normal, const Ray &ray, Vector3 &point) {
		const float facing = normal.Dot(ray.Direction);

		// Near parallel. See the declaration: the crossing exists but is
		// arbitrarily far away and swings for a pixel of movement.
		constexpr float GRAZING = 1e-3f;
		if (std::abs(facing) < GRAZING) {
			return false;
		}

		const float distance = normal.Dot(origin - ray.Origin) / facing;

		// Behind the eye. A plane seen from the other side still has a
		// mathematical crossing and it is one nobody is pointing at.
		if (distance <= 0.0f) {
			return false;
		}

		point = ray.PointAt(distance);
		return true;
	}

	bool ClosestPointOnAxis(Vector3 origin, Vector3 axis, const Ray &ray, float &along) {
		// The classic closest-approach-of-two-lines solve. `axis` and
		// `ray.Direction` are both unit length, so the two diagonal terms are
		// one and the determinant reduces to `1 - cos^2` between them.
		// **From the axis to the ray, not the other way round.** The solve
		// measures `along` outwards from `origin`, so reversing this reverses
		// every drag - the handle follows the cursor in a mirror, which reads as
		// the gizmo being attached to the wrong axis rather than as a sign.
		const Vector3 between = ray.Origin - origin;

		const float axisDotRay = axis.Dot(ray.Direction);
		const float determinant = 1.0f - axisDotRay * axisDotRay;

		// Near parallel: the two lines never meaningfully approach, and the
		// solve below divides by almost nothing. See the declaration - this is
		// the axis-pointing-at-you case, and it has to refuse rather than
		// produce a number.
		constexpr float PARALLEL = 1e-4f;
		if (determinant < PARALLEL) {
			return false;
		}

		const float betweenDotAxis = between.Dot(axis);
		const float betweenDotRay = between.Dot(ray.Direction);

		along = (betweenDotAxis - axisDotRay * betweenDotRay) / determinant;
		return true;
	}

	bool PanelProjection::WorldToPanel(Vector3 world, glm::vec2 &panel) const {
		if (!IsValid()) {
			return false;
		}

		const glm::vec4 clip = Matrix * glm::vec4(world.X, world.Y, world.Z, 1.0f);
		if (clip.w <= NEAR_W) {
			return false;
		}

		// Clip to normalised device coordinates, then NDC to the image rect.
		// **The rect, not the panel** - see the header.
		const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);

		// Y is flipped because NDC counts upwards from the centre and a panel
		// counts downwards from its top edge.
		panel = ImageMin +
				glm::vec2((ndc.x * 0.5f + 0.5f) * ImageSize.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * ImageSize.y);
		return true;
	}

	bool
	PanelProjection::ProjectSegment(Vector3 from, Vector3 to, glm::vec2 &outFrom, glm::vec2 &outTo) const {
		if (!IsValid()) {
			return false;
		}

		glm::vec4 a = Matrix * glm::vec4(from.X, from.Y, from.Z, 1.0f);
		glm::vec4 b = Matrix * glm::vec4(to.X, to.Y, to.Z, 1.0f);

		// The near plane, not an epsilon. See `PanelProjection::Near`.
		const float plane = Near > NEAR_W ? Near : NEAR_W;

		const bool aBehind = a.w <= plane;
		const bool bBehind = b.w <= plane;

		if (aBehind && bBehind) {
			return false;
		}

		// One endpoint crosses. `w` is linear in world position, so the
		// parameter at which it reaches the plane is exact rather than
		// approached - and the interpolated clip-space point is the same one
		// that clipping in world space and re-projecting would give.
		if (aBehind != bBehind) {
			const float t = (plane - a.w) / (b.w - a.w);
			const glm::vec4 crossing = a + (b - a) * t;
			if (aBehind) {
				a = crossing;
			} else {
				b = crossing;
			}
		}

		const auto toPanel = [this](const glm::vec4 &clip) {
			const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
			return ImageMin +
				   glm::vec2(
					   (ndc.x * 0.5f + 0.5f) * ImageSize.x, (1.0f - (ndc.y * 0.5f + 0.5f)) * ImageSize.y
				   );
		};

		outFrom = toPanel(a);
		outTo = toPanel(b);
		return true;
	}

	Ray PanelProjection::PanelToRay(glm::vec2 panel) const {
		if (!IsValid()) {
			return Ray(Eye, Vector3{0.0f, 0.0f, -1.0f});
		}

		// The inverse is computed per call rather than cached on the struct: a
		// cached inverse is a second copy of the matrix that has to be
		// invalidated when the first changes, and a panel issues one ray per
		// click rather than one per pixel. The grid, which does want many
		// projections, goes the other way and never needs this at all.
		const glm::mat4 inverse = glm::inverse(Matrix);

		const glm::vec2 relative = (panel - ImageMin) / ImageSize;
		const glm::vec2 ndc(relative.x * 2.0f - 1.0f, 1.0f - relative.y * 2.0f);

		// Two points down the same pixel, un-projected and subtracted. Taking a
		// single near-plane point and treating it as a direction would be wrong
		// by the eye offset, which is invisible at the centre of the panel and
		// grows towards the corners - the same shape of bug as the image-rect
		// trap, and just as easy to miss.
		const glm::vec4 nearPoint = inverse * glm::vec4(ndc.x, ndc.y, 0.0f, 1.0f);
		const glm::vec4 farPoint = inverse * glm::vec4(ndc.x, ndc.y, 1.0f, 1.0f);

		if (nearPoint.w == 0.0f || farPoint.w == 0.0f) {
			return Ray(Eye, Vector3{0.0f, 0.0f, -1.0f});
		}

		const glm::vec3 from = glm::vec3(nearPoint) / nearPoint.w;
		const glm::vec3 to = glm::vec3(farPoint) / farPoint.w;

		const Vector3 direction = Vector3{to.x - from.x, to.y - from.y, to.z - from.z}.Unit();

		return Ray(Eye, direction);
	}

	bool PanelProjection::ContainsPanel(glm::vec2 panel) const {
		return IsValid() && panel.x >= ImageMin.x && panel.y >= ImageMin.y &&
			   panel.x <= ImageMin.x + ImageSize.x && panel.y <= ImageMin.y + ImageSize.y;
	}

	// How far an oriented box reaches along a direction from its centre.
	//
	// **The support function, which is what "resting on it" needs.** A box
	// laid on a slope touches the slope at one corner, and the distance
	// from the centre to that corner along the surface normal is exactly
	// this sum - using half the height instead would sink a tilted part
	// into the ground by however much it is tilted.
	float SupportAlong(const CFrame &frame, const Vector3 &half, const Vector3 &direction) {
		const Vector3 right = frame.RightVector();
		const Vector3 up = frame.UpVector();
		const Vector3 back = frame.VectorToWorldSpace(Vector3::ZAxis);

		return std::abs(direction.Dot(right)) * half.X + std::abs(direction.Dot(up)) * half.Y +
			   std::abs(direction.Dot(back)) * half.Z;
	}

	// A rotation with its up along a surface normal, turned as little as
	// possible.
	//
	// **The old facing is projected onto the new plane rather than
	// discarded.** A part dropped onto a wall has to keep pointing the way
	// the author left it pointing; rebuilding the basis from the normal
	// alone would spin it to whatever the arbitrary second axis happened to
	// be, and every part dropped on that wall would face the same way
	// whatever the author had done.
	glm::quat AlignedTo(const CFrame &was, const Vector3 &normal) {
		const Vector3 up = normal.Unit();

		Vector3 forward = was.LookVector() - up * was.LookVector().Dot(up);
		if (forward.Magnitude() < 0.001f) {
			// The part was already looking straight at the surface, so its
			// facing says nothing about the new plane. Its right-hand side
			// does, and is the next-best thing it was left pointing at.
			forward = was.RightVector() - up * was.RightVector().Dot(up);
		}
		if (forward.Magnitude() < 0.001f) {
			return was.Rotation();
		}
		forward = forward.Unit();

		// Right-handed, and `LookVector` is `-Z` - so the basis's third
		// column is the *back*. Getting this the other way round mirrors
		// every part that is dropped, which reads as the model being wrong
		// rather than the maths.
		const Vector3 back = forward * -1.0f;
		const Vector3 right = up.Cross(back);

		const glm::mat3 basis(
			glm::vec3(right.X, right.Y, right.Z),
			glm::vec3(up.X, up.Y, up.Z),
			glm::vec3(back.X, back.Y, back.Z)
		);
		return glm::normalize(glm::quat_cast(basis));
	}
}
