#include <engine/core/types/CFrame.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

namespace engine::core {

	namespace {
		glm::vec3 ToGlm(const Vector3 &vector) {
			return { vector.X, vector.Y, vector.Z };
		}

		Vector3 FromGlm(const glm::vec3 &vector) {
			return { vector.x, vector.y, vector.z };
		}
	}

	CFrame CFrame::Angles(float pitch, float yaw, float roll) {
		// Y, then X, then Z, applied intrinsically — the order Roblox's
		// CFrame.Angles uses. Quaternion multiplication is not commutative, so
		// this order is the definition rather than a detail.
		const glm::quat aroundY = glm::angleAxis(yaw, glm::vec3 { 0.0f, 1.0f, 0.0f });
		const glm::quat aroundX = glm::angleAxis(pitch, glm::vec3 { 1.0f, 0.0f, 0.0f });
		const glm::quat aroundZ = glm::angleAxis(roll, glm::vec3 { 0.0f, 0.0f, 1.0f });

		return CFrame(Vector3::Zero, glm::normalize(aroundY * aroundX * aroundZ));
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

	glm::mat4 CFrame::ToMatrix() const {
		glm::mat4 matrix = glm::toMat4(Rotation());
		matrix[3] = glm::vec4(ToGlm(Position), 1.0f);
		return matrix;
	}
}
