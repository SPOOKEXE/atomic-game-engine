#include <engine/core/types/CFrame.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace engine::core {

	namespace {
		glm::vec3 ToGlm(const Vector3 &vector) {
			return {vector.X, vector.Y, vector.Z};
		}

		Vector3 FromGlm(const glm::vec3 &vector) {
			return {vector.x, vector.y, vector.z};
		}
	}

	CFrame CFrame::Angles(float pitch, float yaw, float roll) {
		// Y, then X, then Z, applied intrinsically - the order Roblox's
		// CFrame.Angles uses. Quaternion multiplication is not commutative, so
		// this order is the definition rather than a detail.
		const glm::quat aroundY = glm::angleAxis(yaw, glm::vec3{0.0f, 1.0f, 0.0f});
		const glm::quat aroundX = glm::angleAxis(pitch, glm::vec3{1.0f, 0.0f, 0.0f});
		const glm::quat aroundZ = glm::angleAxis(roll, glm::vec3{0.0f, 0.0f, 1.0f});

		return CFrame(Vector3::Zero, glm::normalize(aroundY * aroundX * aroundZ));
	}

	Vector3 CFrame::ToAngles() const {
		// Derived from `Angles`' own composition rather than taken from a
		// general Euler-extraction routine: R = Ry(yaw) * Rx(pitch) * Rz(roll),
		// so the elements below are the ones that isolate each turn. A different
		// order would extract angles that rebuild a different rotation, which is
		// the failure this function exists to avoid.
		//
		// glm is column-major, so `matrix[column][row]`.
		const glm::mat3 matrix = glm::mat3_cast(glm::normalize(Rotation()));

		// Row 1, column 2 is -sin(pitch) and nothing else.
		const float negativeSinePitch = std::clamp(matrix[2][1], -1.0f, 1.0f);
		const float pitch = std::asin(-negativeSinePitch);

		// cos(pitch) scales every term the yaw and roll extractions use, so near
		// the pole both atan2 calls approach 0/0.
		//
		// **Measured from the terms themselves rather than from `cos(pitch)`,
		// and the threshold is about conditioning rather than about dividing by
		// zero.** atan2 is defined at the origin, so the failure is not a NaN -
		// it is a confidently wrong angle. The first version of this tested
		// `cos(pitch) < 1e-6` and was wrong for a reason worth keeping: at a
		// true pole the quaternion round-trip leaves `sin(pitch)` at
		// 0.99999994 rather than 1, so `cos(pitch)` reads 3.5e-4 - a thousand
		// times the threshold - while the two elements below have already lost
		// every significant bit. A test on the derived angle cannot see that;
		// a test on the operands can.
		//
		// 1e-3 is where float noise in a normalised rotation stops being small
		// against the values carrying the ratio. The nearest angle any caller
		// is likely to sit at is one degree off the pole, where this reads
		// 1.7e-2 - comfortably outside.
		constexpr float NEAR_POLE = 1.0e-3f;
		const float yawTermMagnitude = std::hypot(matrix[2][0], matrix[2][2]);

		if (yawTermMagnitude < NEAR_POLE) {
			// Roll folded into yaw. See the header: the rotation is reproduced
			// exactly, but the split between the two coincident axes is a
			// convention rather than a measurement.
			return Vector3{pitch, std::atan2(-matrix[0][2], matrix[0][0]), 0.0f};
		}

		return Vector3{
			pitch,
			std::atan2(matrix[2][0], matrix[2][2]),
			std::atan2(matrix[0][1], matrix[1][1]),
		};
	}

	CFrame CFrame::LookAt(const Vector3 &from, const Vector3 &to, const Vector3 &up) {
		const Vector3 forward = (to - from).Unit();
		if (forward == Vector3::Zero) {
			// `from` and `to` are the same point. There is no direction to
			// face, so keep the identity rotation rather than produce a NaN.
			return CFrame(from);
		}

		// glm::quatLookAt takes the direction to face down -Z, which is the
		// same convention as LookVector.
		const glm::quat rotation = glm::quatLookAt(ToGlm(forward), ToGlm(up.Unit()));
		return CFrame(from, glm::normalize(rotation));
	}

	CFrame CFrame::operator*(const CFrame &other) const {
		const glm::quat rotation = Rotation();
		const glm::vec3 rotated = rotation * ToGlm(other.Position);

		return CFrame(Position + FromGlm(rotated), glm::normalize(rotation * other.Rotation()));
	}

	Vector3 CFrame::PointToWorldSpace(const Vector3 &point) const {
		return Position + FromGlm(Rotation() * ToGlm(point));
	}

	Vector3 CFrame::VectorToWorldSpace(const Vector3 &vector) const {
		return FromGlm(Rotation() * ToGlm(vector));
	}

	CFrame CFrame::Inverse() const {
		// Conjugate rather than inverse: the quaternion is kept normalised on
		// construction, and for a unit quaternion the two are the same at a
		// fraction of the cost.
		const glm::quat inverse = glm::conjugate(Rotation());
		return CFrame(FromGlm(inverse * -ToGlm(Position)), inverse);
	}

	CFrame CFrame::Lerp(const CFrame &target, float alpha) const {
		return CFrame(
			Position.Lerp(target.Position, alpha),
			glm::normalize(glm::slerp(Rotation(), target.Rotation(), alpha))
		);
	}

	CFrame CFrame::NLerp(const CFrame &target, float alpha) const {
		const glm::quat from = Rotation();
		glm::quat to = target.Rotation();

		// Shortest arc. A quaternion and its negation are the same orientation,
		// so when the two point away from each other the straight line between
		// them passes through the long way round - 359 degrees of rotation to
		// express one degree of difference.
		if (glm::dot(from, to) < 0.0f) {
			to = -to;
		}

		// Componentwise, then renormalised. That is the whole of it: the
		// straight line between two unit quaternions leaves the unit sphere, and
		// pushing it back on is one reciprocal square root rather than the
		// inverse trigonometry slerp needs to stay on it all the way.
		const glm::quat blended{
			from.w + (to.w - from.w) * alpha,
			from.x + (to.x - from.x) * alpha,
			from.y + (to.y - from.y) * alpha,
			from.z + (to.z - from.z) * alpha,
		};

		return CFrame(Position.Lerp(target.Position, alpha), glm::normalize(blended));
	}

	glm::mat4 CFrame::ToMatrix() const {
		glm::mat4 matrix = glm::toMat4(Rotation());
		matrix[3] = glm::vec4(ToGlm(Position), 1.0f);
		return matrix;
	}
}
