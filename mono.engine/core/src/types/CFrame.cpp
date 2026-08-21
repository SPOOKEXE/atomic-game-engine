#include <engine/core/types/CFrame.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

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

	Vector3 CFrame::PointToObjectSpace(const Vector3 &point) const {
		// The conjugate applied to the offset, rather than building `Inverse()`
		// and asking it. Same answer, and this is called per point in a loop
		// where the other form constructs a whole frame to throw away.
		return FromGlm(glm::conjugate(Rotation()) * ToGlm(point - Position));
	}

	Vector3 CFrame::VectorToObjectSpace(const Vector3 &vector) const {
		return FromGlm(glm::conjugate(Rotation()) * ToGlm(vector));
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

	CFrame CFrame::FromEulerAnglesXYZ(float rx, float ry, float rz) {
		// X, then Y, then Z, applied intrinsically - Roblox's `CFrame.Angles`
		// and `CFrame.fromEulerAnglesXYZ`, which are the same function.
		//
		// **The outermost turn is the leftmost factor**, which is the whole
		// difference from `Angles` above: that one is `Y * X * Z` and this is
		// `X * Y * Z`. Quaternion multiplication does not commute, so these are
		// two different rotations from the same three numbers.
		const glm::quat aroundX = glm::angleAxis(rx, glm::vec3{1.0f, 0.0f, 0.0f});
		const glm::quat aroundY = glm::angleAxis(ry, glm::vec3{0.0f, 1.0f, 0.0f});
		const glm::quat aroundZ = glm::angleAxis(rz, glm::vec3{0.0f, 0.0f, 1.0f});

		return CFrame(Vector3::Zero, glm::normalize(aroundX * aroundY * aroundZ));
	}

	Vector3 CFrame::ToEulerAnglesXYZ() const {
		// Derived from `FromEulerAnglesXYZ`' own composition rather than from a
		// general extraction routine, for `ToAngles`' reason: R = Rx * Ry * Rz,
		// so these are the elements that isolate each turn and a different order
		// would extract angles that rebuild a different rotation.
		//
		// glm is column-major, so `matrix[column][row]`.
		const glm::mat3 matrix = glm::mat3_cast(glm::normalize(Rotation()));

		// Row 0, column 2 is sin(ry) and nothing else.
		const float sineY = std::clamp(matrix[2][0], -1.0f, 1.0f);
		const float ry = std::asin(sineY);

		// cos(ry) scales every term the other two extractions use, so near the
		// pole both atan2 calls approach 0/0. Measured from the terms rather
		// than from cos(ry), for `ToAngles`' reason: atan2 is defined at the
		// origin, so the failure is a confidently wrong angle and not a NaN.
		const float cosineY = std::sqrt(matrix[0][0] * matrix[0][0] + matrix[1][0] * matrix[1][0]);
		constexpr float NEAR_POLE = 1e-4f;

		if (cosineY < NEAR_POLE) {
			// Gimbal lock: the X and Z axes coincide and the split between them
			// is arbitrary. Z is taken as zero and the whole turn attributed to
			// X, which reproduces the rotation exactly - it is a choice, not a
			// recovery of information that is no longer there.
			//
			// **`sineY` scales the first term and dropping it inverts the
			// answer.** With R = Rx * Ry * Rz and cos(ry) at zero, R10 is
			// `ca*sc + sa*sb*cc` and R11 is `ca*cc - sa*sb*sc`; multiplying R10
			// by sb - which is ±1 here - collapses both to the sine and cosine
			// of the single combined turn `rx + sb*rz`. Without it the recovered
			// rotation is a mirror of the one that went in, which is a
			// difference no single-axis test can see.
			//
			// **And the pole angle is written down rather than computed.**
			// `asin` is ill-conditioned where its argument approaches one: its
			// derivative goes to infinity there, so a sine that is a hundred
			// nanoseconds of float away from 1.0 - which is what
			// `sin(pi/2)` in single precision actually is - comes back as an
			// angle a few ten-thousandths short. Being *at* the pole is exactly
			// the branch this is, and the angle there is a quarter turn by
			// definition, so it is taken as one.
			constexpr float QUARTER_TURN = std::numbers::pi_v<float> / 2.0f;
			return Vector3{
				std::atan2(sineY * matrix[0][1], matrix[1][1]), std::copysign(QUARTER_TURN, sineY), 0.0f
			};
		}

		return Vector3{std::atan2(-matrix[2][1], matrix[2][2]), ry, std::atan2(-matrix[1][0], matrix[0][0])};
	}

	CFrame CFrame::FromAxisAngle(const Vector3 &axis, float angle) {
		const float length = axis.Magnitude();
		if (length <= 0.0f) {
			// Identity rather than a NaN quaternion, the choice `LookAt` makes
			// for a zero-length sight line: a degenerate input is a rotation of
			// nothing, not a poisoned value that spreads through every multiply.
			return CFrame();
		}
		return CFrame(Vector3::Zero, glm::normalize(glm::angleAxis(angle, ToGlm(axis / length))));
	}

	void CFrame::ToAxisAngle(Vector3 &axis, float &angle) const {
		const glm::quat rotation = glm::normalize(Rotation());

		// **The sign is taken off first.** A quaternion and its negation are the
		// same rotation, but `glm::axis` on the negated one reports the opposite
		// axis and an angle past pi. Flipping to the positive-w half puts every
		// answer in [0, pi] about a consistent axis, which is what Roblox
		// reports and what a caller comparing two results expects.
		const glm::quat shortest = rotation.w < 0.0f ? -rotation : rotation;

		angle = glm::angle(shortest);
		if (angle <= 0.0f) {
			// The identity turns about nothing. `XAxis` rather than a zero
			// vector, so a caller handing the pair straight back to
			// `FromAxisAngle` gets the identity instead of a degenerate axis.
			axis = Vector3::XAxis;
			angle = 0.0f;
			return;
		}
		axis = FromGlm(glm::axis(shortest));
	}

	CFrame CFrame::FromMatrix(const Vector3 &position, const Vector3 &right, const Vector3 &up) {
		// **Orthonormalised rather than trusted.** Three vectors a script
		// computed are almost never exactly orthogonal, and a quaternion built
		// from a skewed basis is a rotation that also scales and shears - which
		// shows up as a part that is subtly the wrong size and nothing pointing
		// at why.
		const float rightLength = right.Magnitude();
		if (rightLength <= 0.0f) {
			return CFrame(position);
		}

		const Vector3 x = right / rightLength;

		// The third column first, because it is the one both inputs agree on:
		// `x` cross `up` is perpendicular to each whatever angle they meet at,
		// where correcting `up` against `x` first would need `up` to be nearly
		// right already.
		const Vector3 forward = x.Cross(up);
		const float forwardLength = forward.Magnitude();
		if (forwardLength <= 0.0f) {
			// Parallel inputs name a plane rather than a basis. Identity
			// rotation, for `FromAxisAngle`'s reason.
			return CFrame(position);
		}

		const Vector3 z = forward / forwardLength;
		const Vector3 y = z.Cross(x);

		glm::mat3 basis;
		basis[0] = ToGlm(x);
		basis[1] = ToGlm(y);
		basis[2] = ToGlm(z);
		return CFrame(position, glm::normalize(glm::quat_cast(basis)));
	}

	CFrame CFrame::FromRotationBetweenVectors(const Vector3 &from, const Vector3 &to) {
		const float fromLength = from.Magnitude();
		const float toLength = to.Magnitude();
		if (fromLength <= 0.0f || toLength <= 0.0f) {
			return CFrame();
		}

		const Vector3 start = from / fromLength;
		const Vector3 finish = to / toLength;
		const float alignment = std::clamp(start.Dot(finish), -1.0f, 1.0f);

		// **Antiparallel has no shortest answer**, and the choice is made here
		// rather than left to whichever way a cross product of two nearly
		// opposite vectors happened to fall - that cross product is near zero,
		// so its direction is noise and two runs on different machines can pick
		// different half-turns.
		constexpr float ANTIPARALLEL = -1.0f + 1e-6f;
		if (alignment <= ANTIPARALLEL) {
			// Any perpendicular does. The axis least aligned with `start` is
			// picked so the cross product is well away from zero.
			const Vector3 seed = std::abs(start.X) < 0.9f ? Vector3::XAxis : Vector3::YAxis;
			const Vector3 axis = start.Cross(seed).Unit();
			return CFrame(Vector3::Zero, glm::normalize(glm::angleAxis(std::acos(-1.0f), ToGlm(axis))));
		}

		return CFrame(Vector3::Zero, glm::normalize(glm::rotation(ToGlm(start), ToGlm(finish))));
	}

	std::array<float, 12> CFrame::GetComponents() const {
		// Row-major, because that is the order the 12-argument `CFrame.new`
		// takes them back in. glm is column-major, so `matrix[column][row]`
		// reads transposed here on purpose.
		const glm::mat3 matrix = glm::mat3_cast(glm::normalize(Rotation()));
		return {
			Position.X,
			Position.Y,
			Position.Z,
			matrix[0][0],
			matrix[1][0],
			matrix[2][0],
			matrix[0][1],
			matrix[1][1],
			matrix[2][1],
			matrix[0][2],
			matrix[1][2],
			matrix[2][2],
		};
	}

	CFrame CFrame::Orthonormalize() const {
		const glm::quat rotation = Rotation();
		if (glm::dot(rotation, rotation) <= 0.0f) {
			// A zero quaternion has no direction to keep, and `glm::normalize`
			// on one is a division by zero rather than an error.
			return CFrame(Position);
		}
		return CFrame(Position, glm::normalize(rotation));
	}

	float CFrame::AngleBetween(const CFrame &other) const {
		const glm::quat first = glm::normalize(Rotation());
		const glm::quat second = glm::normalize(other.Rotation());

		// **The absolute dot product**, because a quaternion and its negation
		// are the same rotation: without it two frames that are identical can
		// report pi apart, which is the largest wrong answer available.
		const float alignment = std::clamp(std::abs(glm::dot(first, second)), 0.0f, 1.0f);
		return 2.0f * std::acos(alignment);
	}

	bool CFrame::FuzzyEq(const CFrame &other, float epsilon) const {
		const Vector3 offset = Position - other.Position;
		if (std::abs(offset.X) > epsilon || std::abs(offset.Y) > epsilon || std::abs(offset.Z) > epsilon) {
			return false;
		}

		// The angle rather than the components, for `AngleBetween`'s reason: a
		// component-wise test calls a quaternion and its negation a mismatch of
		// 2.0 when they are the same orientation.
		return AngleBetween(other) <= epsilon;
	}

	glm::mat4 CFrame::ToMatrix() const {
		glm::mat4 matrix = glm::toMat4(Rotation());
		matrix[3] = glm::vec4(ToGlm(Position), 1.0f);
		return matrix;
	}
}
