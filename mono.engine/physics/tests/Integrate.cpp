#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Integrate.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.physics.integrate")
// The query terms and the fields being written are scene's components, and the
// Motion/RigidBody split is what this system is shaped by.
TEST_DEPENDS("engine.scene.components")
// Iteration, the tick delta and the affinity check are all Store's.
TEST_DEPENDS("engine.ecs.store")
// Position and orientation live in a CFrame, and the quaternion step has to
// leave it a valid one.
TEST_DEPENDS("engine.core.types.cframe")
// The parallel dispatch, its grain, and the timing a case here reads to show
// that an empty world never wakes the pool.
TEST_DEPENDS("engine.parallel.jobs")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::parallel::BatchTiming;
using engine::parallel::Jobs;
using engine::physics::IntegrateMotion;
using engine::scene::Motion;
using engine::scene::RigidBody;
using engine::scene::Transform;

namespace {
	// Sixty hertz, which is what the physics was tuned against.
	constexpr float TICK = 1.0f / 60.0f;

	Store MakeStore(const char *name) {
		engine::scene::RegisterSceneComponents();
		return Store(name);
	}

	Entity Moving(Store &store, const Vector3 &position, const Vector3 &linear, const Vector3 &angular) {
		const Entity entity = store.Create();
		store.Set<Transform>(entity, Transform{CFrame{position}});
		store.Set<Motion>(entity, Motion{linear, angular});
		return entity;
	}

	// The pool, started and joined around the case that needs one.
	//
	// Left running, its workers are still parked when the process tears its
	// statics down and the binary never exits — which reads as a hung test
	// rather than as a missing `Stop`.
	struct Pool {
		explicit Pool(unsigned workers) {
			Jobs::Start(workers);
		}
		~Pool() {
			Jobs::Stop();
		}
	};

	float QuaternionLength(const CFrame &frame) {
		return std::sqrt(
			frame.QuaternionX * frame.QuaternionX + frame.QuaternionY * frame.QuaternionY +
			frame.QuaternionZ * frame.QuaternionZ + frame.QuaternionW * frame.QuaternionW
		);
	}
}

TEST_CASE("a velocity moves a transform by the fixed tick", "[physics][integrate]") {
	Store store = MakeStore("integrate.linear");
	const Entity entity = Moving(store, Vector3::Zero, Vector3{3.0f, 0.0f, -6.0f}, Vector3::Zero);

	store.AdvanceTick(TICK);
	IntegrateMotion(store);

	const Vector3 &position = store.Get<Transform>(entity)->Frame.Position;
	CHECK(position.X == Approx(3.0f * TICK));
	CHECK(position.Y == Approx(0.0f));
	CHECK(position.Z == Approx(-6.0f * TICK));
}

TEST_CASE("the system and the free function are one implementation", "[physics][integrate]") {
	// **The claim `Advanced` is published on.** A replica placing a body the
	// authority has stopped describing calls the free function; a tick calls the
	// system; and a client integrating different arithmetic from the server is a
	// disagreement that only ever surfaces as drift. So the two are asserted to
	// agree bit for bit rather than approximately — they are the same code, and
	// the day they are not this is what says so.
	Store store = MakeStore("integrate.shared");
	const Vector3 linear{3.0f, -1.5f, 0.25f};
	const Vector3 angular{0.4f, -2.0f, 1.25f};

	const CFrame start =
		CFrame(Vector3{1.0f, 2.0f, 3.0f}, engine::core::CFrame::Angles(0.3f, 0.9f, -0.2f).Rotation());
	const Entity entity = Moving(store, start.Position, linear, angular);
	store.GetMutable<Transform>(entity)->Frame = start;

	store.AdvanceTick(TICK);
	IntegrateMotion(store);

	const CFrame &stepped = store.Get<Transform>(entity)->Frame;
	const CFrame direct = engine::physics::Advanced(start, linear, angular, TICK);

	CHECK(stepped.Position.X == direct.Position.X);
	CHECK(stepped.Position.Y == direct.Position.Y);
	CHECK(stepped.Position.Z == direct.Position.Z);
	CHECK(stepped.QuaternionX == direct.QuaternionX);
	CHECK(stepped.QuaternionY == direct.QuaternionY);
	CHECK(stepped.QuaternionZ == direct.QuaternionZ);
	CHECK(stepped.QuaternionW == direct.QuaternionW);

	// And it moved at all, or the equality above would hold with both halves
	// doing nothing.
	CHECK(direct.Position.X == Approx(start.Position.X + linear.X * TICK));
	CHECK(QuaternionLength(direct) == Approx(1.0f));
}

TEST_CASE("integration never loads a mass", "[physics][integrate]") {
	// The whole reason `Motion` and `RigidBody` are separate components. A
	// platform, a projectile and a demo cube all move and none of them has a
	// mass, so an entity with no `RigidBody` has to be integrated — and adding
	// `RigidBody` to the query would silently stop moving all three.
	Store store = MakeStore("integrate.massless");
	const Entity entity = Moving(store, Vector3::Zero, Vector3{1.0f, 0.0f, 0.0f}, Vector3::Zero);

	REQUIRE_FALSE(store.Has<RigidBody>(entity));

	store.AdvanceTick(TICK);
	IntegrateMotion(store);

	CHECK(store.Get<Transform>(entity)->Frame.Position.X == Approx(TICK));
	CHECK_FALSE(store.Has<RigidBody>(entity));
}

TEST_CASE("an angular velocity turns a transform", "[physics][integrate]") {
	// `Motion::Angular` existed with nothing reading it until this system. A
	// spin about world Y has to leave +Y where it is and swing +X toward -Z.
	Store store = MakeStore("integrate.angular");
	const float ratePerSecond = 6.0f;
	const Entity entity = Moving(store, Vector3::Zero, Vector3::Zero, Vector3{0.0f, ratePerSecond, 0.0f});

	store.AdvanceTick(TICK);
	IntegrateMotion(store);

	const CFrame &frame = store.Get<Transform>(entity)->Frame;

	// A first-order step turns by 2 * atan(0.5 * dt * rate), which is the exact
	// angle this integrator produces rather than the dt * rate it approximates.
	// Checking against the approximation would either be loose enough to accept
	// no rotation at all or tight enough to fail on the method rather than on a
	// bug.
	const float expected = 2.0f * std::atan(0.5f * TICK * ratePerSecond);
	const Vector3 right = frame.RightVector();

	CHECK(right.X == Approx(std::cos(expected)).margin(1e-5));
	CHECK(right.Z == Approx(-std::sin(expected)).margin(1e-5));
	CHECK(frame.UpVector().Y == Approx(1.0f).margin(1e-5));
	CHECK(frame.Position == Vector3::Zero);
}

TEST_CASE("a spinning transform keeps a unit quaternion", "[physics][integrate]") {
	// A first-order rotation step leaves the quaternion off the unit sphere and
	// the error compounds. The symptom is not a rotation bug: a `CFrame` whose
	// quaternion is not unit length *scales* what it transforms, so parts
	// slowly grow and nobody looks at the integrator.
	Store store = MakeStore("integrate.normalised");
	const Entity entity = Moving(store, Vector3::Zero, Vector3::Zero, Vector3{1.0f, 4.0f, -2.0f});

	store.AdvanceTick(TICK);
	for (int tick = 0; tick < 600; tick++) {
		IntegrateMotion(store);
	}

	CHECK(QuaternionLength(store.Get<Transform>(entity)->Frame) == Approx(1.0f).margin(1e-5));
}

TEST_CASE("a still transform does not drift", "[physics][integrate]") {
	// Zero velocity has to be exactly zero motion. A normalise that divided by
	// a slightly-off magnitude, or a position add that accumulated, would show
	// as a world that creeps while nothing is moving.
	Store store = MakeStore("integrate.still");
	const Entity entity = Moving(store, Vector3{5.0f, -3.0f, 2.0f}, Vector3::Zero, Vector3::Zero);

	store.AdvanceTick(TICK);
	for (int tick = 0; tick < 100; tick++) {
		IntegrateMotion(store);
	}

	const CFrame &frame = store.Get<Transform>(entity)->Frame;
	CHECK(frame.Position == Vector3{5.0f, -3.0f, 2.0f});
	CHECK(frame.QuaternionW == Approx(1.0f));
}

TEST_CASE("zero entities does nothing and never wakes the pool", "[physics][integrate]") {
	const Pool pool(2);

	// A sentinel dispatch with enough work in it to register on the clock, so
	// that "the timing did not move" is a claim about a dispatch that did not
	// happen rather than about two zeroes.
	//
	// `Jobs::For` returns before touching the timing when the count is zero, so
	// what this catches is a dispatch with real work in it — a scratch pass
	// over `CountMatching`, say, or a fixed-size loop that runs whether or not
	// there is anything to integrate.
	volatile uint64_t sink = 0;
	Jobs::For(1, 1, [&sink](size_t, size_t) {
		for (uint64_t spin = 0; spin < 400000; spin++) {
			sink = sink + spin;
		}
	});

	const BatchTiming before = Jobs::LastBatch();
	REQUIRE(before.WallMilliseconds > 0.0f);

	Store store = MakeStore("integrate.empty");
	REQUIRE(store.CountMatching<Transform>() == 0);

	store.AdvanceTick(TICK);
	IntegrateMotion(store);

	const BatchTiming after = Jobs::LastBatch();
	CHECK(after.WallMilliseconds == before.WallMilliseconds);
	CHECK(after.BusyMilliseconds == before.BusyMilliseconds);
	CHECK(after.Participants == before.Participants);
	CHECK(store.CountMatching<Transform>() == 0);
}

TEST_CASE("two runs of one scene integrate to identical bytes", "[physics][integrate]") {
	// Same binary, same platform, same result. The parallel path partitions
	// rows within a table and each range writes only its own rows, so a run
	// that happened to be split differently across workers must still produce
	// the same floats — which is what makes a recorded run replay.
	const auto run = [](const char *name) {
		Store store = MakeStore(name);
		for (uint32_t index = 0; index < 200; index++) {
			const float offset = static_cast<float>(index);
			Moving(
				store,
				Vector3{offset, -offset, offset * 0.5f},
				Vector3{0.5f, offset * 0.01f, -0.25f},
				Vector3{0.1f, offset * 0.02f, -0.3f}
			);
		}

		store.AdvanceTick(TICK);
		for (int tick = 0; tick < 30; tick++) {
			IntegrateMotion(store);
		}

		std::vector<float> written;
		written.reserve(200 * 7);
		store.Each<const Transform>([&written](Entity, const Transform &transform) {
			const CFrame &frame = transform.Frame;
			written.push_back(frame.Position.X);
			written.push_back(frame.Position.Y);
			written.push_back(frame.Position.Z);
			written.push_back(frame.QuaternionX);
			written.push_back(frame.QuaternionY);
			written.push_back(frame.QuaternionZ);
			written.push_back(frame.QuaternionW);
		});
		return written;
	};

	const std::vector<float> first = run("integrate.determinism.a");
	const std::vector<float> second = run("integrate.determinism.b");

	REQUIRE(first.size() == second.size());
	CHECK(first == second);
}
