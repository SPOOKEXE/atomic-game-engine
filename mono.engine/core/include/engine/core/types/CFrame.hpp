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
// `OrientedBoxBounds` at the bottom is the one free function here, and it is
// here because it is the only box operation that needs a rotation. Its comment
// carries the measurement that moved it.
//
// @tier L1 · shared

#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <array>
#include <cmath>

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
		explicit CFrame(const Vector3 &position) : Position(position) {}

		// Constructs a transform and stores the supplied quaternion without normalising it.
		//
		// @param position Translation in the containing space, in the caller's distance unit.
		// @param rotation A unit quaternion describing the orientation.
		CFrame(const Vector3 &position, const glm::quat &rotation)
			: Position(position), QuaternionX(rotation.x), QuaternionY(rotation.y), QuaternionZ(rotation.z),
			  QuaternionW(rotation.w) {}

		// Returns the stored orientation as a GLM quaternion without normalising it.
		glm::quat Rotation() const {
			return glm::quat(QuaternionW, QuaternionX, QuaternionY, QuaternionZ);
		}

		// Constructs an origin-centred rotation from intrinsic Y, then X, then Z turns.
		//
		// **This is Roblox's `CFrame.fromEulerAnglesYXZ`, which it also spells
		// `CFrame.fromOrientation`. It is NOT Roblox's `CFrame.Angles`**, and
		// this comment said it was until v0.18. `CFrame.Angles` is an alias for
		// `fromEulerAnglesXYZ` and composes X, then Y, then Z; `FromEulerAnglesXYZ`
		// below is that one. Quaternion multiplication does not commute, so the
		// two build different rotations from the same three numbers and a script
		// pasted from Roblox turned the wrong way with nothing saying why.
		//
		// The order here is right for what actually uses it: `BasePart.Orientation`
		// is YXZ in Roblox too, and `scene::OrientationProperty` reads `ToAngles`.
		// The name was the error, not the maths.
		//
		// @param pitch Rotation about X, in radians.
		// @param yaw   Rotation about Y, in radians.
		// @param roll  Rotation about Z, in radians.
		static CFrame Angles(float pitch, float yaw, float roll);

		// Constructs an origin-centred rotation from intrinsic X, then Y, then Z turns.
		//
		// **Roblox's `CFrame.Angles` and `CFrame.fromEulerAnglesXYZ`, which are
		// the same function.** Kept apart from `Angles` above rather than
		// replacing it, because the two are both wanted: this one is what a
		// ported script means, and the other is what `Orientation` round-trips
		// through.
		//
		// @param rx Rotation about X, in radians.
		// @param ry Rotation about Y, in radians.
		// @param rz Rotation about Z, in radians.
		// @since v0.18
		static CFrame FromEulerAnglesXYZ(float rx, float ry, float rz);

		// Constructs an origin-centred rotation of `angle` radians about `axis`.
		//
		// A zero-length axis yields the identity rather than a NaN quaternion,
		// which is the same choice `LookAt` makes for a zero-length sight line:
		// a degenerate input is a rotation of nothing, not a poisoned value that
		// spreads through every multiply that touches it.
		//
		// @param axis  The direction to turn about. Normalised here, so any
		//              non-zero length works.
		// @param angle How far to turn, in radians.
		// @since v0.18
		static CFrame FromAxisAngle(const Vector3 &axis, float angle);

		// Constructs a frame from a position and three basis directions.
		//
		// **Roblox's `CFrame.fromMatrix`, and the 12-argument `CFrame.new`
		// reaches the same place.** That one is given a row-major rotation
		// matrix, so its X column is `{R00, R10, R20}` - the caller does that
		// transpose, and this takes columns.
		//
		// The basis is orthonormalised here rather than trusted: three vectors a
		// script computed are almost never exactly orthogonal, and a quaternion
		// built from a skewed basis is a rotation that also scales and shears.
		// `right` is kept as given, `up` is made perpendicular to it, and the
		// third is their cross product.
		//
		// @param position Where the frame sits.
		// @param right    Local +X, in world space. Must be non-zero.
		// @param up       Local +Y, in world space. Must not be parallel to `right`.
		// @return The frame, or the identity rotation at `position` if the two
		//         directions are degenerate.
		// @since v0.18
		static CFrame FromMatrix(const Vector3 &position, const Vector3 &right, const Vector3 &up);

		// Constructs the shortest origin-centred rotation carrying `from` onto `to`.
		//
		// Roblox's `CFrame.fromRotationBetweenVectors`. Antiparallel inputs have
		// no shortest answer - every half-turn about any perpendicular axis works
		// - so one perpendicular is chosen and the choice is stated rather than
		// left to whichever way a cross product happened to fall.
		//
		// @param from A direction, normalised here.
		// @param to   Where it should end up, normalised here.
		// @since v0.18
		static CFrame FromRotationBetweenVectors(const Vector3 &from, const Vector3 &to);

		// Recovers the intrinsic Y-X-Z turns this rotation was built from.
		//
		// The inverse of `Angles`, and it has to be exactly that inverse: the
		// script surface exposes an `Orientation` a caller can read, modify and
		// assign back, so `part.Orientation = part.Orientation` must not drift.
		// Any other Euler order round-trips to a different rotation.
		//
		// Translation is ignored; only the rotation is decomposed.
		//
		// **Gimbal lock is resolved rather than left to produce a NaN.** When
		// pitch reaches ±90° the yaw and roll axes coincide and the split
		// between them is arbitrary, so roll is taken as zero and the whole turn
		// is attributed to yaw. That is a choice, not a recovery of lost
		// information - the rotation is reproduced exactly either way.
		//
		// @return Rotation about X, Y and Z as `{pitch, yaw, roll}`, in radians.
		Vector3 ToAngles() const;

		// Recovers the intrinsic X-Y-Z turns this rotation was built from.
		//
		// The inverse of `FromEulerAnglesXYZ`, and Roblox's `ToEulerAnglesXYZ`.
		// Gimbal lock is resolved the way `ToAngles` resolves it: at the pole the
		// split between the first and third turns is arbitrary, so the third is
		// taken as zero and the whole turn goes to the first. The rotation is
		// reproduced exactly either way.
		//
		// @return Rotation about X, Y and Z, in radians.
		// @since v0.18
		Vector3 ToEulerAnglesXYZ() const;

		// Recovers the axis and angle this rotation turns about.
		//
		// Roblox's `ToAxisAngle`. The identity has no meaningful axis, so it
		// reports `{XAxis, 0}` rather than a zero vector - a caller feeding the
		// result straight back to `FromAxisAngle` gets the identity either way,
		// and a zero axis would be a direction nothing can normalise.
		//
		// @param axis  Written with the unit axis of rotation.
		// @param angle Written with the turn about it, in radians, in [0, pi].
		// @since v0.18
		void ToAxisAngle(Vector3 &axis, float &angle) const;

		// The twelve numbers Roblox's `GetComponents` reports, in its order.
		//
		// `{x, y, z, R00, R01, R02, R10, R11, R12, R20, R21, R22}`, with the
		// rotation **row-major** because that is the order the 12-argument
		// `CFrame.new` takes them back in. Round-tripping through
		// `FromMatrix` therefore needs the transpose, which `FromMatrix` says.
		//
		// @since v0.18
		std::array<float, 12> GetComponents() const;

		// This frame's rotation at the world origin.
		//
		// Roblox's `CFrame.Rotation`. Named `RotationOnly` here because
		// `Rotation()` already means "the stored quaternion" on this type, and
		// two members one letter apart returning different kinds of thing is a
		// mistake waiting for a tired reader.
		//
		// @since v0.18
		CFrame RotationOnly() const {
			return CFrame(Vector3::Zero, Rotation());
		}

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

		// Transforms a world-space point into this frame's local space.
		//
		// Roblox's `PointToObjectSpace`. The inverse of `PointToWorldSpace`, and
		// computed directly rather than as `Inverse().PointToWorldSpace(...)` -
		// that form builds a whole frame to throw it away, and this is called per
		// point in a loop.
		//
		// @since v0.18
		Vector3 PointToObjectSpace(const Vector3 &point) const;

		// Transforms a world-space direction into this frame's local space.
		//
		// Roblox's `VectorToObjectSpace`. Rotation only, so a translation of the
		// frame does not move a direction.
		//
		// @since v0.18
		Vector3 VectorToObjectSpace(const Vector3 &vector) const;

		// Composes `other` as if it were expressed in this frame's local space.
		//
		// Roblox's `ToWorldSpace`, and identical to `*this * other`. Both names
		// exist because both read naturally at different call sites: the operator
		// where a chain of transforms is being built, this where one frame is
		// being reinterpreted.
		//
		// @since v0.18
		CFrame ToWorldSpace(const CFrame &other) const {
			return *this * other;
		}

		// Expresses `other` relative to this frame.
		//
		// Roblox's `ToObjectSpace`. The inverse of `ToWorldSpace`: a frame handed
		// back through both is the frame it started as.
		//
		// @since v0.18
		CFrame ToObjectSpace(const CFrame &other) const {
			return Inverse() * other;
		}

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
		// Returns the world-space direction of local +Z, Roblox's `ZVector`.
		//
		// **The opposite of `LookVector`, and both are wanted.** Roblox names the
		// three basis columns `XVector`, `YVector` and `ZVector`, and its `ZVector`
		// is +Z where its `LookVector` is -Z - the same pair, spelled for two
		// different jobs. `RightVector` and `UpVector` are already `XVector` and
		// `YVector`; this is the third, and without it a script reading all three
		// columns has to negate one of them and know why.
		//
		// @since v0.18
		Vector3 ZVector() const {
			return VectorToWorldSpace(Vector3::ZAxis);
		}

		// Interpolates position linearly and orientation spherically at constant angular speed.
		//
		// `alpha` 0 and 1 select the endpoint poses. No clamping is performed, and
		// both endpoint rotations must be unit quaternions.
		//
		// Costs an `acos` and three `sin` calls. When the two orientations are
		// close together - which is what interpolating between consecutive ticks
		// means - NLerp is the same answer for a fraction of the price. Reach for
		// this one when the endpoints are far apart and the *rate* has to be
		// uniform, such as an animation blend over a whole second.
		CFrame Lerp(const CFrame &target, float alpha) const;

		// Interpolates position linearly and orientation by normalised linear
		// interpolation, taking the shortest arc.
		//
		// The same endpoints as Lerp and the same path through space, but not at
		// the same speed: the angular rate eases toward the midpoint instead of
		// holding constant. The error is bounded by the angle between the
		// endpoints and vanishes with it - under about 30 degrees it is well
		// below what a pixel can show, and two consecutive simulation ticks are
		// nowhere near that far apart.
		//
		// What it buys is the transcendentals. Lerp is an `acos` and three `sin`
		// calls per call; this is a multiply-add, a reciprocal square root and a
		// sign test. On a frame interpolating thousands of transforms that is the
		// difference between the interpolation being the most expensive thing in
		// the frame and it not appearing on the list.
		//
		// The shortest-arc flip is the part Lerp does not do and this one must:
		// without it, two orientations more than 180 degrees apart interpolate
		// the long way round, and a cube spinning gently appears to snap
		// backwards once per revolution.
		//
		// `alpha` 0 and 1 select the endpoint poses. No clamping is performed, and
		// both endpoint rotations must be unit quaternions.
		CFrame NLerp(const CFrame &target, float alpha) const;

		// Returns a copy whose stored quaternion is unit length.
		//
		// Roblox's `Orthonormalize`, and it means something narrower here.
		// Roblox stores a 3x3 matrix, which drifts out of orthogonality as it is
		// multiplied and needs the basis rebuilt. This stores a quaternion, which
		// cannot shear at all - the only drift it can accumulate is length - so
		// the whole of the repair is a normalise.
		//
		// A zero quaternion has no direction to keep and yields the identity.
		//
		// @since v0.18
		CFrame Orthonormalize() const;

		// Whether two frames are the same to within `epsilon`.
		//
		// Roblox's `FuzzyEq`. Position is compared per component; rotation is
		// compared as the **angle between them** rather than component by
		// component, because a quaternion and its negation are the same rotation
		// and a component-wise test calls those two a mismatch of 2.0.
		//
		// @param other   The frame to compare against.
		// @param epsilon The tolerance, in the caller's distance unit for
		//                position and in radians for rotation.
		// @since v0.18
		bool FuzzyEq(const CFrame &other, float epsilon) const;

		// The angle between this frame's rotation and another's, in radians.
		//
		// Roblox's `AngleBetween`. Always in [0, pi]: the shortest turn that
		// carries one orientation onto the other, whichever sign the two stored
		// quaternions happen to carry.
		//
		// @since v0.18
		float AngleBetween(const CFrame &other) const;

		// Returns the local-to-world transform as a column-major matrix.
		//
		// Translation occupies the fourth column, ready for a uniform buffer.
		glm::mat4 ToMatrix() const;
	};

	// Returns the world-space box that encloses a rotated local box.
	//
	// The extent grows by the **absolute value** of the rotated half-extent on
	// each axis, which is what makes a unit cube turned 45 degrees about Y come
	// out root two wide. Rotating only the centre and keeping the original
	// extent is a cheaper function that is also wrong: it produces a bound
	// smaller than the shape, and a broad phase whose bound is too small drops
	// contacts without reporting anything.
	//
	// **Here rather than on `AABB`, and the reason is what a header costs.** It
	// is the only box operation that needs a rotation, so a static member of
	// `AABB` would make `AABB.hpp` include this file - and a box, which is six
	// floats, would arrive at all 152 of its objects carrying a quaternion and
	// `<glm/gtc/quaternion.hpp>` behind it. Measured with the real compile
	// flags: `AABB.hpp` cost 73,835 preprocessed lines that way against this
	// file's 73,713, which is a box that was 99.8% transform. It is 25,368
	// free of it, a 66% cut, and what remains is `<cmath>` reached through
	// `Vector3.hpp` rather than anything of its own.
	//
	// The dependency also only runs one way round: a frame knows what a box
	// is, and a box has no reason to know what a frame is.
	//
	// @param frame      Where the box is and how it is turned.
	// @param halfExtent The box's reach from its own centre, in local axes.
	// @since v0.19
	inline AABB OrientedBoxBounds(const CFrame &frame, const Vector3 &halfExtent) {
		// Most authored parts are axis aligned. Avoid constructing and applying the
		// same identity quaternion three times in every culling and broadphase walk.
		if (frame.QuaternionX == 0.0f && frame.QuaternionY == 0.0f && frame.QuaternionZ == 0.0f) {
			return AABB::FromCentre(frame.Position, halfExtent);
		}

		const Vector3 right = frame.VectorToWorldSpace(Vector3::XAxis);
		const Vector3 up = frame.VectorToWorldSpace(Vector3::YAxis);
		const Vector3 forward = frame.VectorToWorldSpace(Vector3::ZAxis);

		// Each world axis takes a contribution from all three local axes: this
		// is the absolute value of the rotation matrix applied to the
		// half-extent, written out because the matrix is not stored.
		const Vector3 worldHalfExtent{
			std::abs(right.X) * halfExtent.X + std::abs(up.X) * halfExtent.Y +
				std::abs(forward.X) * halfExtent.Z,
			std::abs(right.Y) * halfExtent.X + std::abs(up.Y) * halfExtent.Y +
				std::abs(forward.Y) * halfExtent.Z,
			std::abs(right.Z) * halfExtent.X + std::abs(up.Z) * halfExtent.Y +
				std::abs(forward.Z) * halfExtent.Z,
		};
		return AABB::FromCentre(frame.Position, worldHalfExtent);
	}
}
