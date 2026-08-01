#pragma once

// A rigid transform: a position and an orientation, with no scale.
//
// This stores a quaternion plus position. A 3x3 rotation matrix would need
// re-orthonormalising after enough multiplications; a quaternion needs one
// normalise and is half the size, which matters when it is a component in an
// ECS column read once per entity per frame.
//
// No scale. Scale belongs to whatever is being drawn, not to where it is.
//
// @tier L1 · shared

#include <engine/core/types/Vector3.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

namespace engine::core {

	// A right-handed, Y-up rigid transform with -Z as its forward direction.
	//
	// Position uses the same caller-chosen unit as transformed points. Orientation
	// is stored as quaternion components in X, Y, Z, W order; a valid rigid
	// transform keeps that quaternion at unit length.
	struct CFrame {
		// Translation in the containing coordinate space, in the caller's distance unit.
		Vector3 Position;
		// Quaternion X component.
		float QuaternionX = 0.0f;
		// Quaternion Y component.
		float QuaternionY = 0.0f;
		// Quaternion Z component.
		float QuaternionZ = 0.0f;
		// Quaternion W component; 1 completes the identity rotation by default.
		float QuaternionW = 1.0f;

		// Constructs the identity transform at the world origin.
		CFrame() = default;

		// Constructs a translation with identity rotation.
		explicit CFrame(const Vector3 &position)
			: Position(position) {
		}

		// Constructs a transform and stores the supplied quaternion without normalising it.
		//
		// @param position Translation in the containing space, in the caller's distance unit.
		// @param rotation A unit quaternion describing the orientation.
		CFrame(const Vector3 &position, const glm::quat &rotation)
			: Position(position)
			, QuaternionX(rotation.x)
			, QuaternionY(rotation.y)
			, QuaternionZ(rotation.z)
			, QuaternionW(rotation.w) {
		}

		// Returns the stored orientation as a GLM quaternion without normalising it.
		glm::quat Rotation() const {
			return glm::quat(QuaternionW, QuaternionX, QuaternionY, QuaternionZ);
		}

		// Constructs an origin-centred rotation from intrinsic Y, then X, then Z turns.
		//
		// @param pitch Rotation about X, in radians.
		// @param yaw   Rotation about Y, in radians.
		// @param roll  Rotation about Z, in radians.
		static CFrame Angles(float pitch, float yaw, float roll);

		// Constructs a frame at `from` whose -Z direction points toward `to`.
		//
		// Equal `from` and `to` positions produce identity rotation at `from` rather
		// than a NaN quaternion.
		//
		// @param from The world-space position in the caller's distance unit.
		// @param to   The world-space target in the same unit.
		// @param up   The preferred +Y direction; it must be non-zero and not parallel
		//             to the sight line.
		static CFrame LookAt(const Vector3 &from, const Vector3 &to, const Vector3 &up = Vector3::YAxis);

		// Composes `other` inside this frame, applying `other` before this transform.
		//
		// Transforming a point by the result matches transforming it by `other` and
		// then by this frame.
		CFrame operator*(const CFrame &other) const;

		// Transforms a local-space point to world space with rotation and translation.
		Vector3 PointToWorldSpace(const Vector3 &point) const;
		// Transforms a local-space vector to world space with rotation only.
		Vector3 VectorToWorldSpace(const Vector3 &vector) const;

		// Returns the transform that maps world points back into this frame's local space.
		//
		// The stored rotation must be a unit quaternion.
		CFrame Inverse() const;

		// Returns the world-space direction of local +X.
		Vector3 RightVector() const {
			return VectorToWorldSpace(Vector3::XAxis);
		}
		// Returns the world-space direction of local +Y.
		Vector3 UpVector() const {
			return VectorToWorldSpace(Vector3::YAxis);
		}
		// Returns the world-space direction of local -Z, which is camera-forward.
		Vector3 LookVector() const {
			return VectorToWorldSpace(-Vector3::ZAxis);
		}

		// Interpolates position linearly and orientation spherically at constant angular speed.
		//
		// `alpha` 0 and 1 select the endpoint poses. No clamping is performed, and
		// both endpoint rotations must be unit quaternions.
		CFrame Lerp(const CFrame &target, float alpha) const;

		// Returns the local-to-world transform as a column-major matrix.
		//
		// Translation occupies the fourth column, ready for a uniform buffer.
		glm::mat4 ToMatrix() const;
	};
}
