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

#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
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
using engine::scene::SurfaceAppearance;
using engine::scene::Tags;
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
			return SpawnLooking(Visual{});
		}

		// The same, with the appearance the authority sent.
		Entity SpawnLooking(const Visual &visual) {
			const Entity entity = World.Create();
			World.Set<Transform>(entity, Transform{});
			World.Set<Bounds>(entity, Bounds{Vector3{0.5f, 0.5f, 0.5f}});
			World.Set<Visual>(entity, visual);
			return entity;
		}

		// The same again, with v0.9's two components on it as well.
		//
		// **Separate from `SpawnLooking` on purpose**, because the case that
		// matters is a replica receiving a `Visual` and *not* these — a server
		// that has not been taught to send them, which was every server until
		// they were added to the authority lists. That case has to keep working
		// and is what `SpawnLooking` covers.
		Entity SpawnSurfaced(const Visual &visual, const SurfaceAppearance &appearance, uint32_t tags) {
			const Entity entity = SpawnLooking(visual);
			World.Set<SurfaceAppearance>(entity, appearance);
			World.Set<Tags>(entity, Tags{tags});
			return entity;
		}

		const std::vector<engine::scene::DrawInstance> &Instances() const {
			return World.Resource<DrawList>()->Instances;
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

// **A replica draws what a part *looks like*, not only where it is.**
//
// `CollectReplicated` builds a `DrawInstance` field by field, and for three
// releases it copied the first five and stopped — so `Transparency`, `Surface`
// and later `CastShadow` arrived over the wire, sat correctly in the store, and
// were dropped on the way to the renderer. A glass pane replicated as solid and
// a mirror replicated as a plain part, while every property panel and every
// script that inspected them said the values were right.
//
// That is the same class of loss as a serialiser forgetting a field, arriving
// through a different door, and it is why this asserts every field rather than
// the one that was last found missing.
TEST_CASE("a replica draws every field of what it was sent", "[client][replication]") {
	Replica replica;

	Visual sent;
	sent.Tint = engine::core::Color3{0.25f, 0.5f, 0.75f};
	sent.Mesh = engine::core::Name("replicated_test.Mesh");
	sent.Material = engine::core::Name("replicated_test.Material");
	sent.Transparency = 0.5f;
	sent.Surface = 1;
	sent.CastShadow = false;

	replica.SpawnLooking(sent);
	replica.Draw();

	REQUIRE(replica.Instances().size() == 1);
	const engine::scene::DrawInstance &drawn = replica.Instances()[0];

	CHECK(drawn.Tint.R == Approx(sent.Tint.R));
	CHECK(drawn.Tint.G == Approx(sent.Tint.G));
	CHECK(drawn.Tint.B == Approx(sent.Tint.B));
	CHECK(drawn.Mesh == sent.Mesh);
	CHECK(drawn.Material == sent.Material);
	CHECK(drawn.Transparency == Approx(sent.Transparency));
	CHECK(drawn.Surface == sent.Surface);
	CHECK_FALSE(drawn.CastShadow);

	// **The two v0.9 added, and their defaults are the ones that matter here.**
	// This entity was spawned without either component — which is exactly a
	// server that has not been taught to send them — so the draw instance must
	// come out with an invalid texture and no tags rather than with whatever the
	// last row happened to hold.
	CHECK_FALSE(drawn.Texture.IsValid());
	CHECK(drawn.TagMask == 0);
	CHECK(drawn.Alpha == engine::scene::AlphaMode::Opaque);
}

TEST_CASE("a replica draws the surface appearance and tags it was sent", "[client][replication]") {
	// The other half of the case above: an authority that *does* send them.
	// Without this the imported meshes v0.9 added arrive on a replica with a
	// mesh name and no texture name, which is half a model and reads as a
	// broken texture path rather than as a component nobody sent.
	Replica replica;

	Visual sent;
	sent.Mesh = engine::core::Name("replicated_test.Fox");

	SurfaceAppearance appearance;
	appearance.ColourMap = engine::core::Name("replicated_test.FoxTexture");
	appearance.Mode = engine::scene::AlphaMode::Clip;
	appearance.AlphaCutoff = 0.4f;

	replica.SpawnSurfaced(sent, appearance, 0b101);
	replica.Draw();

	REQUIRE(replica.Instances().size() == 1);
	const engine::scene::DrawInstance &drawn = replica.Instances()[0];

	CHECK(drawn.Mesh == sent.Mesh);
	CHECK(drawn.Texture == appearance.ColourMap);
	CHECK(drawn.Alpha == engine::scene::AlphaMode::Clip);

	// **The mask crosses and the names do not.** A `TagTable` is a resource and
	// resources have no wire form, so a replica cannot say what bit one is —
	// but a surface camera's filter and this mask both came from one authority,
	// so comparing them is still meaningful.
	CHECK(drawn.TagMask == 0b101);
}

// **`Visible` is the one half of the render gate a replica can honour**, and it
// has to honour it here rather than through `scene::Rendered`.
//
// The gate proper is an ancestry test, and ancestry is what the wire does not
// carry — `Server.cpp` replicates `Transform`, `Motion`, `Bounds` and `Visual`,
// and `Hierarchy` holds entity handles that mean nothing until they are remapped
// between two processes' directories. So a replica has no tree to test and the
// authority's decision about what is in the scene arrives as *what it sent*.
//
// `Visible` rides inside `Visual`, so this process genuinely was told, and a
// hidden part must not be drawn.
TEST_CASE("a replica does not draw a part it was told is invisible", "[client][replication]") {
	Replica replica;

	replica.Spawn();

	Visual hidden;
	hidden.Visible = false;
	replica.SpawnLooking(hidden);

	replica.Draw();
	CHECK(replica.Instances().size() == 1);
}

// **The registration this world used to skip**, and the failure it caused was a
// long way from the cause.
//
// `Components::Of<T>` caches its answer per type per process and marks the name
// it minted as automatic — so the first mention of `DrawList` anywhere decides
// what it is called. `BuildReplicatedWorld` reached for the resource without
// registering first, which named it `client::DrawList`, the compiler's
// spelling. Nothing failed here. It failed in whichever world was built *next*,
// where the explicit `RegisterClientComponents` aborted the process naming a
// type that function never mentions.
//
// This suite runs in its own process, so `Replica` above is the first thing in
// it to touch `DrawList` — which is what makes this assertion mean anything.
TEST_CASE("a replicated world registers its own types before it uses them", "[client][replication]") {
	Replica replica;

	const engine::ecs::ComponentId id = engine::ecs::Components::Of<DrawList>();
	REQUIRE(id.IsValid());

	// The explicit name, not the compiler's. A recording carries this string.
	CHECK(engine::ecs::Components::Describe(id).Name.Text() == "client.DrawList");
}
