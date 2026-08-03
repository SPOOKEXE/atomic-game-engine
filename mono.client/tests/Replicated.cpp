// Drawing a world this process does not own, and not writing to it.
//
// The buffer itself is `engine.replication.snapshotbuffer`'s, and everything
// about the delay, the stall and the predicted entity is stated there. What is
// this client's own is the seam: which component carries a pose, when the store
// holds a tick in full, and — the one that matters most — that the interpolated
// result reaches a `DrawInstance` and nothing else.
//
// **The negative case is the important one.** Interpolation is presentation. A
// render-rate quantity that reached a component would make the world this
// process replicates depend on the frame rate of whoever happened to be
// watching it, which is the same rule `v02v03v04.md` §2.11 states for views.
//
// Headless. Nothing here needs a GPU, and drawing the line there is what keeps
// the suite runnable everywhere.

#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("client.replicated")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.drawinstance")

using Catch::Approx;
using client::DrawList;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::replication::InterpolationSettings;
using engine::replication::SnapshotBuffer;
using engine::scene::Bounds;
using engine::scene::Transform;
using engine::scene::Visual;

namespace {
	constexpr double TICK_RATE = 60.0;
	constexpr int FRAMES_PER_TICK = 4;
	constexpr float FRAME_SECONDS = 1.0f / static_cast<float>(TICK_RATE * FRAMES_PER_TICK);

	// A replicated world with nothing in it, built the way `Client` builds one.
	struct Replica {
		Replica() {
			engine::parallel::Jobs::Start(1);

			InterpolationSettings interpolation;
			interpolation.TickRate = TICK_RATE;
			client::BuildReplicatedWorld(World, Systems, interpolation);
		}

		~Replica() {
			engine::parallel::Jobs::Stop();
		}

		Replica(const Replica &) = delete;
		Replica &operator=(const Replica &) = delete;

		// One drawable row, at the origin.
		Entity Spawn() {
			const Entity entity = World.Create();
			World.Set<Transform>(entity, Transform{});
			World.Set<Bounds>(entity, Bounds{Vector3{0.5f, 0.5f, 0.5f}});
			World.Set<Visual>(entity, Visual{});
			return entity;
		}

		// What arriving looks like from this side: the connection wrote the
		// tick's values into the store, and then the tick was recorded.
		void Receive(uint64_t tick, Entity entity, float x) {
			World.GetMutable<Transform>(entity)->Frame = CFrame(Vector3{x, 0.0f, 0.0f});
			client::RecordReplicatedTick(World, tick);
		}

		// One frame: exactly what `World::Present` does.
		void Draw() {
			World.SetFrame(FRAME_SECONDS, 0.0f);
			Systems.RunPhases(World, Phase::PreRender, Phase::PreRender);
		}

		float Drawn(size_t index = 0) const {
			return World.Resource<DrawList>()->Instances[index].Frame.Position.X;
		}

		Store World{"client.replica.test"};
		Scheduler Systems;
	};
}

TEST_CASE("a replicated world is built with a draw list and a snapshot buffer", "[client][replication]") {
	Replica replica;
	REQUIRE(replica.World.Resource<DrawList>() != nullptr);
	REQUIRE(replica.World.Resource<SnapshotBuffer>() != nullptr);

	// And nothing that simulates. Everything in this world arrived.
	replica.Systems.RunPhases(replica.World, Phase::PreSimulation, Phase::PostSimulation);
	REQUIRE(replica.World.Resource<DrawList>()->Instances.empty());
}

TEST_CASE("what is drawn is interpolated between two received ticks", "[client][replication]") {
	Replica replica;
	const Entity entity = replica.Spawn();

	for (uint64_t tick = 1; tick <= 12; tick++) {
		replica.Receive(tick, entity, static_cast<float>(tick));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			replica.Draw();
		}
	}

	// Four frames per tick, and the drawn position moves on every one of them
	// by a quarter of a tick. This is the assertion that the judder is gone.
	float previous = replica.Drawn();
	for (uint64_t tick = 13; tick <= 20; tick++) {
		replica.Receive(tick, entity, static_cast<float>(tick));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			replica.Draw();
			const float drawn = replica.Drawn();
			REQUIRE(drawn - previous == Approx(1.0f / FRAMES_PER_TICK).margin(0.01));
			previous = drawn;
		}
	}
}

TEST_CASE("nothing interpolated reaches the store", "[client][replication]") {
	// **The negative test, and the one that matters.** The interpolated pose
	// exists for exactly as long as it takes to become a `DrawInstance`. A
	// `Transform` carrying it would be a component written at the frame rate in
	// a world whose whole premise is that this process derives nothing.
	Replica replica;
	const Entity entity = replica.Spawn();

	const uint64_t tickBefore = replica.World.Time().Tick;

	for (uint64_t tick = 1; tick <= 16; tick++) {
		replica.Receive(tick, entity, static_cast<float>(tick));
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			replica.Draw();

			// The row still holds exactly what the server last said, to the
			// bit — not the interpolated value, and not something rounded
			// through it.
			REQUIRE(replica.World.Get<Transform>(entity)->Frame.Position.X == static_cast<float>(tick));
		}
	}

	// And the drawn position is genuinely a different number, or the check
	// above would pass because nothing is interpolating at all.
	REQUIRE(replica.Drawn() != Approx(16.0f));
	REQUIRE(replica.Drawn() < 16.0f);

	// The world's own clock did not move either. `PreRender` sets the frame
	// fields and a tick sets the rest; a presentation pass that advanced the
	// tick would be this process simulating.
	REQUIRE(replica.World.Time().Tick == tickBefore);

	// No `PreviousTransform` was invented on the way, which is the fix the
	// deferred entry exists to rule out: it interpolates between whichever two
	// packets happened to land rather than between two ticks.
	REQUIRE_FALSE(replica.World.Has<engine::scene::PreviousTransform>(entity));
}

TEST_CASE("an entity with no buffered pose is drawn where it is", "[client][replication]") {
	// A row that arrived in a structural message this frame has no history yet,
	// and the honest thing to draw is the only pose there is. The alternative —
	// not drawing it — is a hole in the frame.
	Replica replica;
	const Entity entity = replica.Spawn();
	replica.World.GetMutable<Transform>(entity)->Frame = CFrame(Vector3{42.0f, 0.0f, 0.0f});

	replica.Draw();

	REQUIRE(replica.World.Resource<DrawList>()->Instances.size() == 1);
	REQUIRE(replica.Drawn() == Approx(42.0f));
}

TEST_CASE("nothing is buffered before the join", "[client][replication]") {
	// **Tick zero is not a tick.** It is what `Connector::Applied` reads for
	// every poll before the joining snapshot has landed, and there is no state
	// behind it — the world is empty and the rows it names do not exist yet.
	// Recording it would start the render clock at a tick that never happened,
	// and the first real tick of a server that has been up for a while would
	// then look like a pause of however long that was.
	Replica replica;
	const Entity entity = replica.Spawn();

	auto *buffer = replica.World.ResourceMutable<SnapshotBuffer>();
	REQUIRE(buffer != nullptr);

	for (int poll = 0; poll < 10; poll++) {
		replica.Receive(0, entity, 5.0f);
		replica.Draw();
	}
	REQUIRE(buffer->Stats().Ticks == 0);
	REQUIRE(buffer->Newest() == 0);
	REQUIRE_FALSE(buffer->Holds(0));

	// Drawn all the same, at the pose it holds. A client that showed nothing
	// until it had two ticks would flicker into existence a frame late.
	REQUIRE(replica.Drawn() == Approx(5.0f));

	// The join lands, and from there every poll of that frame finds the same
	// applied tick.
	for (int poll = 0; poll < 10; poll++) {
		replica.Receive(7, entity, 7.0f);
	}
	REQUIRE(buffer->Stats().Ticks == 1);
	REQUIRE(buffer->Newest() == 7);
}
