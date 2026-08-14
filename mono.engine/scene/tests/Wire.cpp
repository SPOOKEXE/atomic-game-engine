// The grid, and the bound it promises.
//
// `Wire.hpp` states an error in metres and an error in radians. This is the
// half that checks them, because a stated bound nobody measures is the same
// thing as a hope with a constant beside it - and the two failure modes it
// guards against pull in opposite directions: a grid quietly widened past the
// bound, and a bound quietly widened past the grid. Every case here therefore
// asserts both that nothing exceeds the stated figure and that something comes
// close to it.

#include <engine/core/Bytes.hpp>
#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.scene.wire")

using engine::core::ByteReader;
using engine::core::ByteWriter;
using engine::core::CFrame;
using engine::core::Random;
using engine::core::Vector3;
using engine::scene::Motion;
using engine::scene::Transform;

namespace scene_wire_test {

	// One value through the compact form and back, which is what a client ends
	// up holding.
	Transform RoundTrip(const Transform &transform) {
		const engine::ecs::WireFormat wire = engine::scene::TransformWire();

		ByteWriter writer;
		wire.Write(writer, &transform, 1);
		REQUIRE(writer.Size() == engine::scene::WIRE_TRANSFORM_BYTES);

		ByteReader reader(writer.Bytes());
		Transform decoded;
		wire.Read(reader, &decoded, 1);
		REQUIRE_FALSE(reader.Failed());
		return decoded;
	}

	Motion RoundTrip(const Motion &motion) {
		const engine::ecs::WireFormat wire = engine::scene::MotionWire();

		ByteWriter writer;
		wire.Write(writer, &motion, 1);
		REQUIRE(writer.Size() == engine::scene::WIRE_MOTION_BYTES);

		ByteReader reader(writer.Bytes());
		Motion decoded;
		wire.Read(reader, &decoded, 1);
		REQUIRE_FALSE(reader.Failed());
		return decoded;
	}

	// The angle between two orientations, in radians. `q` and `-q` are the same
	// rotation, so the dot product is taken absolutely - comparing components
	// would report a full turn for two identical orientations.
	float AngleBetween(const CFrame &left, const CFrame &right) {
		const float dot = left.QuaternionX * right.QuaternionX + left.QuaternionY * right.QuaternionY +
						  left.QuaternionZ * right.QuaternionZ + left.QuaternionW * right.QuaternionW;
		return 2.0f * std::acos(std::min(1.0f, std::abs(dot)));
	}

	float Length(const CFrame &frame) {
		return std::sqrt(
			frame.QuaternionX * frame.QuaternionX + frame.QuaternionY * frame.QuaternionY +
			frame.QuaternionZ * frame.QuaternionZ + frame.QuaternionW * frame.QuaternionW
		);
	}

	CFrame Unit(float x, float y, float z, float w) {
		const float length = std::sqrt(x * x + y * y + z * z + w * w);
		CFrame frame;
		frame.QuaternionX = x / length;
		frame.QuaternionY = y / length;
		frame.QuaternionZ = z / length;
		frame.QuaternionW = w / length;
		return frame;
	}
}

using namespace scene_wire_test;

// --- what it costs ---------------------------------------------------------

TEST_CASE("a transform crosses in ten bytes and a motion in twelve", "[scene][wire]") {
	// **The whole point of the exercise, and the number every budget in
	// `replication` is sized against.** Stated in the header as a promise, so
	// it is measured here rather than trusted: a change that added a field to
	// the encoding would otherwise be found by a client reading one entity's
	// value out of the next entity's bytes.
	CHECK(sizeof(Transform) == 28);
	CHECK(sizeof(Motion) == 24);

	CHECK(engine::scene::TransformWire().Size == 10);
	CHECK(engine::scene::MotionWire().Size == 12);

	Transform transform;
	ByteWriter writer;
	engine::scene::TransformWire().Write(writer, &transform, 1);
	CHECK(writer.Size() == 10);

	// Fixed width, because `Authority` slices `ComponentDelta::Values` by a
	// stride it computed from the first row. A form whose size depended on its
	// value would put every entity after the first at the wrong offset.
	Transform far;
	far.Frame = CFrame(Vector3{-63.0f, 12.5f, 7.25f}, CFrame::Angles(0.4f, 1.1f, -2.0f).Rotation());
	writer.Clear();
	engine::scene::TransformWire().Write(writer, &far, 1);
	CHECK(writer.Size() == 10);
}

// --- position --------------------------------------------------------------

TEST_CASE("a position anywhere in the world decodes inside the stated bound", "[scene][wire]") {
	// Swept across the whole extent rather than sampled near the origin, which
	// is the case a fixed-point grid is always right about.
	const float extent = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;
	const float bound = engine::scene::WIRE_POSITION_ERROR_METRES;

	float worst = 0.0f;
	for (uint32_t step = 0; step <= 4096; step++) {
		const float alpha = static_cast<float>(step) / 4096.0f;
		const float value = -extent + alpha * 2.0f * extent;

		Transform transform;
		transform.Frame.Position = Vector3{value, -value, value * 0.5f};

		const Transform decoded = RoundTrip(transform);
		worst = std::max(worst, std::abs(decoded.Frame.Position.X - value));
		worst = std::max(worst, std::abs(decoded.Frame.Position.Y + value));
		worst = std::max(worst, std::abs(decoded.Frame.Position.Z - value * 0.5f));
	}

	// Inside the bound, and close enough to it that the bound describes this
	// grid rather than some coarser one it would also survive.
	CHECK(worst <= bound);
	CHECK(worst > bound * 0.5f);
}

TEST_CASE("the extremes of the world are exact, and the origin is exact", "[scene][wire]") {
	// **The far wall is where a fixed-point grid usually goes wrong**, because
	// the obvious encoding spends its last code on the negative side and leaves
	// the positive extent a whole step short. `Bounce` clamps entities to
	// exactly this value, so it is the common case rather than an edge one.
	const float extent = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;

	Transform corner;
	corner.Frame.Position = Vector3{extent, -extent, 0.0f};

	const Transform decoded = RoundTrip(corner);
	CHECK(decoded.Frame.Position.X == extent);
	CHECK(decoded.Frame.Position.Y == -extent);
	CHECK(decoded.Frame.Position.Z == 0.0f);
}

TEST_CASE("an entity outside the world is clamped to it, never wrapped", "[scene][wire]") {
	// **Stated behaviour, and the one that has to be a test rather than a
	// sentence.** A wrapped coordinate puts an entity at the far side of the
	// world, which is indistinguishable from a teleport the server meant; a
	// clamped one piles up against a wall somebody can see and locate.
	const float extent = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;

	Transform beyond;
	beyond.Frame.Position = Vector3{extent + 40.0f, -extent - 1000.0f, extent * 3.0f};

	const Transform decoded = RoundTrip(beyond);
	CHECK(decoded.Frame.Position.X == extent);
	CHECK(decoded.Frame.Position.Y == -extent);
	CHECK(decoded.Frame.Position.Z == extent);

	// Not wrapped: every one of them kept its sign and stayed on the side of
	// the world it was on, which is exactly what wrapping would not do.
	CHECK(decoded.Frame.Position.X > 0.0f);
	CHECK(decoded.Frame.Position.Y < 0.0f);
}

TEST_CASE("a decode is monotone, so an entity never overtakes itself", "[scene][wire]") {
	// A grid that decoded out of order would show as an entity that jitters
	// backwards while moving forwards, which reads as a physics bug a long way
	// from here.
	float previous = -1000.0f;
	for (uint32_t step = 0; step <= 8192; step++) {
		const float alpha = static_cast<float>(step) / 8192.0f;
		const float value = -80.0f + alpha * 160.0f;

		Transform transform;
		transform.Frame.Position = Vector3{value, 0.0f, 0.0f};

		const float decoded = RoundTrip(transform).Frame.Position.X;
		REQUIRE(decoded >= previous);
		previous = decoded;
	}
}

TEST_CASE("a decoded position is already on the grid", "[scene][wire]") {
	// **This is what makes a hash of replicated state sound**, which is the
	// reason D00015 puts quantisation before group signatures: encoding a value
	// that has already been decoded has to give the same code back, or the two
	// ends would agree about the value and disagree about its encoding.
	for (uint32_t step = 0; step < 1024; step++) {
		Transform transform;
		transform.Frame.Position = Vector3{
			Random::Range(step, 11u, -70.0f, 70.0f),
			Random::Range(step, 12u, -70.0f, 70.0f),
			Random::Range(step, 13u, -70.0f, 70.0f)
		};

		const Transform once = RoundTrip(transform);
		const Transform twice = RoundTrip(once);

		REQUIRE(twice.Frame.Position.X == once.Frame.Position.X);
		REQUIRE(twice.Frame.Position.Y == once.Frame.Position.Y);
		REQUIRE(twice.Frame.Position.Z == once.Frame.Position.Z);
	}
}

// --- rotation --------------------------------------------------------------

TEST_CASE("smallest-three reconstructs a unit quaternion", "[scene][wire]") {
	float worstLength = 0.0f;
	float worstAngle = 0.0f;

	for (uint32_t sample = 0; sample < 20000; sample++) {
		const CFrame source = Unit(
			Random::Range(sample, 21u, -1.0f, 1.0f),
			Random::Range(sample, 22u, -1.0f, 1.0f),
			Random::Range(sample, 23u, -1.0f, 1.0f),
			Random::Range(sample, 24u, -1.0f, 1.0f)
		);

		Transform transform;
		transform.Frame = source;

		const CFrame decoded = RoundTrip(transform).Frame;
		worstLength = std::max(worstLength, std::abs(Length(decoded) - 1.0f));
		worstAngle = std::max(worstAngle, AngleBetween(source, decoded));
	}

	CHECK(worstLength < 1e-5f);
	CHECK(worstAngle <= engine::scene::WIRE_ROTATION_ERROR_RADIANS);

	// And the bound is about this grid. Without this line a bound ten times
	// too wide would pass every case above.
	CHECK(worstAngle > engine::scene::WIRE_ROTATION_ERROR_RADIANS * 0.5f);
}

TEST_CASE("two nearly equal components pick a largest and stay inside the bound", "[scene][wire]") {
	// **The case that picks the wrong largest.** When two components are within
	// a rounding error of each other the encoder's choice can differ from what
	// a reader would call largest - and it does not matter, because the decoder
	// is told which one was dropped rather than working it out. What would
	// matter is the accuracy, so that is what is asserted.
	const float half = 0.5f;
	const float nudge = 1e-7f;

	const std::vector<CFrame> awkward{
		Unit(half, half, half, half),
		Unit(half + nudge, half, half, half),
		Unit(half, half + nudge, half, half),
		Unit(-half, half, half, half),
		Unit(half, -half, -half, half),
		Unit(-half, -half, -half, -half),
		Unit(0.7071068f, 0.7071068f, 0.0f, 0.0f),
		Unit(0.0f, 0.7071068f, 0.7071068f, 0.0f),
		Unit(0.7071068f, 0.0f, 0.0f, 0.7071068f),
	};

	for (const CFrame &source : awkward) {
		Transform transform;
		transform.Frame = source;

		const CFrame decoded = RoundTrip(transform).Frame;
		CHECK(std::abs(Length(decoded) - 1.0f) < 1e-5f);
		CHECK(AngleBetween(source, decoded) <= engine::scene::WIRE_ROTATION_ERROR_RADIANS);
	}
}

TEST_CASE("the sign of the dropped component does not change the rotation", "[scene][wire]") {
	// **`q` and `-q` are the same rotation**, and the encoding leans on it: the
	// whole quaternion is negated so the dropped component is positive and its
	// sign never has to be sent. A decoder that reconstructed the wrong sign
	// would produce a rotation reflected about the axis, which looks like an
	// entity facing backwards rather than like a rounding error.
	for (uint32_t sample = 0; sample < 4096; sample++) {
		const CFrame source = Unit(
			Random::Range(sample, 31u, -1.0f, 1.0f),
			Random::Range(sample, 32u, -1.0f, 1.0f),
			Random::Range(sample, 33u, -1.0f, 1.0f),
			Random::Range(sample, 34u, -1.0f, 1.0f)
		);

		CFrame negated;
		negated.QuaternionX = -source.QuaternionX;
		negated.QuaternionY = -source.QuaternionY;
		negated.QuaternionZ = -source.QuaternionZ;
		negated.QuaternionW = -source.QuaternionW;

		Transform positive;
		positive.Frame = source;
		Transform opposite;
		opposite.Frame = negated;

		// Both describe the same orientation, so both must decode to the same
		// orientation - and to the same bytes, because a hash over replicated
		// state cannot afford two encodings of one value.
		const CFrame first = RoundTrip(positive).Frame;
		const CFrame second = RoundTrip(opposite).Frame;

		REQUIRE(first.QuaternionX == second.QuaternionX);
		REQUIRE(first.QuaternionY == second.QuaternionY);
		REQUIRE(first.QuaternionZ == second.QuaternionZ);
		REQUIRE(first.QuaternionW == second.QuaternionW);
		REQUIRE(AngleBetween(source, first) <= engine::scene::WIRE_ROTATION_ERROR_RADIANS);
	}
}

TEST_CASE("any four bytes decode to a unit quaternion", "[scene][wire]") {
	// Every field of an inbound message is hostile. A word no encoder would
	// produce - three components that do not fit inside one rotation - still
	// has to come out as something every consumer downstream may treat as a
	// rotation, rather than as a quaternion of length 1.22 that quietly scales
	// whatever it is applied to.
	const engine::ecs::WireFormat wire = engine::scene::TransformWire();

	for (uint32_t sample = 0; sample < 8192; sample++) {
		ByteWriter writer;
		writer.WriteInt16(0);
		writer.WriteInt16(0);
		writer.WriteInt16(0);
		writer.WriteUInt32(Random::Bits(41u, sample));

		ByteReader reader(writer.Bytes());
		Transform decoded;
		wire.Read(reader, &decoded, 1);

		REQUIRE_FALSE(reader.Failed());
		REQUIRE(std::abs(Length(decoded.Frame) - 1.0f) < 1e-5f);
	}
}

// --- motion ----------------------------------------------------------------

TEST_CASE("a velocity decodes inside its own stated bound", "[scene][wire]") {
	// Coarser than the position grid on purpose, and the number that justifies
	// it is the position error rather than zero: over one tick of a 60 Hz world
	// this error moves an entity by a fraction of a position step.
	const float linearReach = engine::scene::WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND;
	const float angularReach = engine::scene::WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND;

	float worstLinear = 0.0f;
	float worstAngular = 0.0f;

	for (uint32_t step = 0; step <= 4096; step++) {
		const float alpha = static_cast<float>(step) / 4096.0f;

		Motion motion;
		motion.Linear =
			Vector3{-linearReach + alpha * 2.0f * linearReach, linearReach * (alpha - 0.5f), alpha * 3.0f};
		motion.Angular =
			Vector3{-angularReach + alpha * 2.0f * angularReach, angularReach * (0.5f - alpha), -alpha};

		const Motion decoded = RoundTrip(motion);
		worstLinear = std::max(worstLinear, std::abs(decoded.Linear.X - motion.Linear.X));
		worstLinear = std::max(worstLinear, std::abs(decoded.Linear.Y - motion.Linear.Y));
		worstLinear = std::max(worstLinear, std::abs(decoded.Linear.Z - motion.Linear.Z));
		worstAngular = std::max(worstAngular, std::abs(decoded.Angular.X - motion.Angular.X));
		worstAngular = std::max(worstAngular, std::abs(decoded.Angular.Y - motion.Angular.Y));
		worstAngular = std::max(worstAngular, std::abs(decoded.Angular.Z - motion.Angular.Z));
	}

	CHECK(worstLinear <= engine::scene::WIRE_LINEAR_ERROR_METRES_PER_SECOND);
	CHECK(worstLinear > engine::scene::WIRE_LINEAR_ERROR_METRES_PER_SECOND * 0.5f);
	CHECK(worstAngular <= engine::scene::WIRE_ANGULAR_ERROR_RADIANS_PER_SECOND);
	CHECK(worstAngular > engine::scene::WIRE_ANGULAR_ERROR_RADIANS_PER_SECOND * 0.5f);
}

TEST_CASE("a velocity error is far below the position grid within one tick", "[scene][wire]") {
	// **The justification for `Motion` having a coarser grid than `Transform`,
	// written as an assertion rather than as a paragraph.** If this ever stops
	// holding, the velocity grid has been widened past the point where its
	// error is invisible in the position it produces.
	const float tick = 1.0f / 60.0f;
	const float travel = engine::scene::WIRE_LINEAR_ERROR_METRES_PER_SECOND * tick;
	CHECK(travel < engine::scene::WIRE_POSITION_ERROR_METRES * 0.25f);
}

TEST_CASE("dead-reckoned error grows with time rather than with the grid", "[scene][wire]") {
	// **The bound `D00015(c)` says nobody had measured, measured.**
	// Interpolating between two decoded poses keeps the error inside
	// `WIRE_POSITION_ERROR_METRES` whatever the elapsed time. *Integrating* a
	// decoded velocity does not: the error is the pose's own plus the
	// velocity's times the seconds since, so the bound is a function of time.
	//
	// Both halves are asserted, in the shape the rest of this file uses.
	// Nothing exceeds the stated growth, and the figure genuinely grows with the
	// elapsed time rather than sitting at the grid's own error - or the horizon
	// below would be describing something that does not happen.
	const float positionReach = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;
	const float linearReach = engine::scene::WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND;

	const float elapsedSeconds[] = {0.0f, 1.0f / 60.0f, 0.1f, engine::scene::WIRE_DEAD_RECKON_SECONDS, 1.0f};

	// **Summed in double, so the figure is the quantisation error and nothing
	// else.** The same sum in float at 64 m from the origin lands on the wrong
	// side of a rounding step often enough to exceed the bound by an ulp -
	// 3.5 um measured, against a quantisation term four hundred times larger.
	// That is a fact about float addition rather than about the grid, and
	// mixing the two into one number would make neither of them checkable.
	double previousWorst = -1.0;
	for (const float seconds : elapsedSeconds) {
		double worst = 0.0;

		for (uint32_t step = 0; step <= 4096; step++) {
			const float alpha = static_cast<float>(step) / 4096.0f;

			Transform placed;
			placed.Frame = CFrame(
				Vector3{
					-positionReach + alpha * 2.0f * positionReach,
					positionReach * (alpha - 0.5f),
					alpha * 7.0f
				}
			);

			Motion moving;
			moving.Linear = Vector3{
				-linearReach + alpha * 2.0f * linearReach, linearReach * (0.5f - alpha), alpha * 11.0f
			};

			const Vector3 place = placed.Frame.Position;
			const Vector3 speed = moving.Linear;
			const Vector3 decodedPlace = RoundTrip(placed).Frame.Position;
			const Vector3 decodedSpeed = RoundTrip(moving).Linear;

			const auto driftOn = [seconds](float truth, float velocity, float held, float heldVelocity) {
				const double guessed = static_cast<double>(held) +
									   static_cast<double>(heldVelocity) * static_cast<double>(seconds);
				const double exact =
					static_cast<double>(truth) + static_cast<double>(velocity) * static_cast<double>(seconds);
				return std::abs(guessed - exact);
			};

			worst = std::max(worst, driftOn(place.X, speed.X, decodedPlace.X, decodedSpeed.X));
			worst = std::max(worst, driftOn(place.Y, speed.Y, decodedPlace.Y, decodedSpeed.Y));
			worst = std::max(worst, driftOn(place.Z, speed.Z, decodedPlace.Z, decodedSpeed.Z));
		}

		const double bound = static_cast<double>(engine::scene::WIRE_POSITION_ERROR_METRES) +
							 static_cast<double>(engine::scene::WIRE_LINEAR_ERROR_METRES_PER_SECOND) *
								 static_cast<double>(seconds);

		INFO("elapsed " << seconds << " s: worst " << worst << " m against a bound of " << bound);
		CHECK(worst <= bound);

		// Growing, and growing with the time rather than with anything else.
		CHECK(worst > previousWorst);
		previousWorst = worst;
	}

	// **The horizon is the equality, not a preference.** Half a step of the
	// velocity grid over a quarter of a second is half a step of the position
	// grid, because the step counts cancel and 64 m over 256 m/s is what is
	// left. Exact in a float: both are a division by the same integer scaled by
	// a power of two.
	CHECK(
		engine::scene::WIRE_LINEAR_ERROR_METRES_PER_SECOND * engine::scene::WIRE_DEAD_RECKON_SECONDS ==
		engine::scene::WIRE_POSITION_ERROR_METRES
	);
	CHECK(engine::scene::WIRE_DEAD_RECKON_SECONDS == 0.25f);
}

TEST_CASE("a velocity outside its grid is clamped", "[scene][wire]") {
	Motion fast;
	fast.Linear = Vector3{9000.0f, -9000.0f, 0.0f};
	fast.Angular = Vector3{500.0f, -500.0f, 0.0f};

	const Motion decoded = RoundTrip(fast);
	CHECK(decoded.Linear.X == engine::scene::WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND);
	CHECK(decoded.Linear.Y == -engine::scene::WIRE_LINEAR_HALF_EXTENT_METRES_PER_SECOND);
	CHECK(decoded.Angular.X == engine::scene::WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND);
	CHECK(decoded.Angular.Y == -engine::scene::WIRE_ANGULAR_HALF_EXTENT_RADIANS_PER_SECOND);
}

// --- the world it is a grid over -------------------------------------------

TEST_CASE("the grid is checked against the world rather than assumed", "[scene][wire]") {
	// The default world and the grid are one decision, and this is where the
	// two constants meet. A `WorldBounds` authored past the grid is a world
	// whose entities pile up against a wall that is not its own, which
	// `WireCoversWorld` is for saying at the point the size is chosen.
	const engine::scene::WorldBounds defaults;
	CHECK(engine::scene::WireCoversWorld(defaults.HalfExtent));

	CHECK(engine::scene::WireCoversWorld(engine::scene::WIRE_POSITION_HALF_EXTENT_METRES));
	CHECK_FALSE(engine::scene::WireCoversWorld(4000.0f));
}

TEST_CASE("a position code no encoder emits still decodes inside the world", "[scene][wire]") {
	// **The one code a symmetric grid leaves unused**, arriving from a peer.
	// A decoder that trusted its input would put an entity a step outside the
	// extent this module states everything is inside - small, and exactly the
	// sort of small that makes a containment test somewhere else disagree with
	// a header.
	const engine::ecs::WireFormat wire = engine::scene::TransformWire();

	ByteWriter writer;
	writer.WriteInt16(-32768);
	writer.WriteInt16(-32768);
	writer.WriteInt16(32767);
	writer.WriteUInt32(0);

	ByteReader reader(writer.Bytes());
	Transform decoded;
	wire.Read(reader, &decoded, 1);
	REQUIRE_FALSE(reader.Failed());

	const float extent = engine::scene::WIRE_POSITION_HALF_EXTENT_METRES;
	CHECK(decoded.Frame.Position.X == -extent);
	CHECK(decoded.Frame.Position.Y == -extent);
	CHECK(decoded.Frame.Position.Z == extent);
}
