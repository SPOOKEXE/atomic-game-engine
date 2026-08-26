// Covers the replica presentation seam without a GPU.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("client.replicated")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.drawinstance")
TEST_DEPENDS("engine.scene.attachments")

using Catch::Approx;
using engine::core::CFrame;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Phase;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::render::DrawList;
using engine::replication::InterpolationSettings;
using engine::replication::SnapshotBuffer;
using engine::scene::Attachment;
using engine::scene::Bounds;
using engine::scene::SurfaceAppearance;
using engine::scene::Tags;
using engine::scene::Transform;
using engine::scene::Visual;

namespace {
	constexpr double TICK_RATE = 60.0;
	constexpr int FRAMES_PER_TICK = 4;
	constexpr float FRAME_SECONDS = 1.0f / static_cast<float>(TICK_RATE * FRAMES_PER_TICK);

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

		Entity Spawn() {
			return SpawnLooking(Visual{});
		}

		Entity SpawnLooking(const Visual &visual) {
			const Entity entity = World.Create();
			World.Set<Transform>(entity, Transform{});
			World.Set<Bounds>(entity, Bounds{Vector3{0.5f, 0.5f, 0.5f}});
			World.Set<Visual>(entity, visual);
			return entity;
		}

		// Keep the missing-component case separate from the full appearance case.
		Entity SpawnSurfaced(const Visual &visual, const SurfaceAppearance &appearance, uint32_t tags) {
			const Entity entity = SpawnLooking(visual);
			World.Set<SurfaceAppearance>(entity, appearance);
			World.Set<Tags>(entity, Tags{tags});
			return entity;
		}

		const std::vector<engine::scene::DrawInstance> &Instances() const {
			return World.Resource<DrawList>()->Instances;
		}

		// A body the authority is also telling this client the velocity of.
		Entity SpawnMoving(float metresPerSecond) {
			const Entity entity = Spawn();
			World.Set<engine::scene::Motion>(
				entity, engine::scene::Motion{Vector3{metresPerSecond, 0.0f, 0.0f}, Vector3{}}
			);
			return entity;
		}

		// What arriving looks like from this side: the connection wrote the
		// tick's values into the store, and then the tick was recorded.
		void Receive(uint64_t tick, Entity entity, float x) {
			World.GetMutable<Transform>(entity)->Frame = CFrame(Vector3{x, 0.0f, 0.0f});
			client::RecordReplicatedTick(World, tick);
		}

		// A body arriving at a steady speed, so that the pose at a tick and the
		// speed on the row describe the same motion.
		void ReceiveMoving(uint64_t tick, Entity entity, float metresPerSecond) {
			Receive(tick, entity, metresPerSecond * static_cast<float>(tick) / static_cast<float>(TICK_RATE));
		}

		void DrawFrames(int frames) {
			for (int frame = 0; frame < frames; frame++) {
				Draw();
			}
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
			// bit - not the interpolated value, and not something rounded
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
	// and the honest thing to draw is the only pose there is. The alternative -
	// not drawing it - is a hole in the frame.
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
	// behind it - the world is empty and the rows it names do not exist yet.
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
// releases it copied the first five and stopped - so `Transparency`, `Surface`
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
	CHECK(drawn.Transparency == Approx(sent.Transparency));
	CHECK(drawn.Surface == sent.Surface);
	CHECK_FALSE(drawn.CastShadow);

	// **The two v0.9 added, and their defaults are the ones that matter here.**
	// This entity was spawned without either component - which is exactly a
	// server that has not been taught to send them - so the draw instance must
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
	appearance.Mode = engine::scene::AlphaMode::Transparency;
	appearance.AlphaCutoff = 0.4f;

	replica.SpawnSurfaced(sent, appearance, 0b101);
	replica.Draw();

	REQUIRE(replica.Instances().size() == 1);
	const engine::scene::DrawInstance &drawn = replica.Instances()[0];

	CHECK(drawn.Mesh == sent.Mesh);
	CHECK(drawn.Texture == appearance.ColourMap);
	CHECK(drawn.Alpha == engine::scene::AlphaMode::Transparency);

	// **The mask crosses and the names do not.** A `TagTable` is a resource and
	// resources have no wire form, so a replica cannot say what bit one is -
	// but a surface camera's filter and this mask both came from one authority,
	// so comparing them is still meaningful.
	CHECK(drawn.TagMask == 0b101);
}

// **`Visible` is the one half of the render gate a replica can honour**, and it
// has to honour it here rather than through `scene::Rendered`.
//
// The gate proper is an ancestry test, and ancestry is what the wire does not
// carry - `Server.cpp` replicates `Transform`, `Motion`, `Bounds` and `Visual`,
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

TEST_CASE("a replica does not publish fully transparent parts", "[client][replication]") {
	Replica replica;
	replica.Spawn();

	Visual authored;
	authored.Transparency = 1.0f;
	replica.SpawnLooking(authored);

	const Entity locallyHidden = replica.Spawn();
	replica.World.Set(locallyHidden, engine::scene::LocalTransparency{1.0f});

	replica.Draw();
	CHECK(replica.Instances().size() == 1);
}

// **The registration this world used to skip**, and the failure it caused was a
// long way from the cause.
//
// `Components::Of<T>` caches its answer per type per process and marks the name
// it minted as automatic - so the first mention of `DrawList` anywhere decides
// what it is called. `BuildReplicatedWorld` reached for the resource without
// registering first, which named it `engine::render::DrawList`, the compiler's
// spelling. Nothing failed here. It failed in whichever world was built *next*,
// where the explicit `RegisterClientComponents` aborted the process naming a
// type that function never mentions.
//
// This suite runs in its own process, so `Replica` above is the first thing in
// it to touch `DrawList` - which is what makes this assertion mean anything.
TEST_CASE("a replicated world registers its own types before it uses them", "[client][replication]") {
	Replica replica;

	const engine::ecs::ComponentId id = engine::ecs::Components::Of<DrawList>();
	REQUIRE(id.IsValid());

	// The explicit name, not the compiler's. A recording carries this string.
	CHECK(engine::ecs::Components::Describe(id).Name.Text() == "client.DrawList");
}

// --- what a snapshot of a replica world can carry -----------------------------

// **Rule 4, and `engine::render::DrawList` learned it the expensive way.** A resource is
// keyed by a component id, and `Store::SetResource` mints one under whatever the
// compiler spells the type as unless somebody registered a name. Nothing notices
// until a world holding it is saved - which is exactly what the studio's Play
// does, and what its Stop restores from.
//
// `DrawList` had no registration at all before v0.7 and `Store::Save` refused
// the world for it. `SnapshotBuffer` is set as a resource on every replica the
// studio holds, and it is the second type in the same position.
TEST_CASE("a replica world can be snapshotted", "[client][replicated]") {
	Replica replica;

	engine::core::ByteWriter writer;
	CHECK(replica.World.Save(writer));
}

// --- dead reckoning, D00015(c) -----------------------------------------------

// **`replication` measures the guess and this file decides who gets one.** The
// buffer knows how long it has been unable to interpolate; only a caller with
// components in front of it knows which rows carry a velocity the authority
// sent and which carry a `NetworkOwner` saying somebody else already simulates
// them. Everything below is that decision, asserted as positions.

namespace {
	// A steady one metre per second, which is slow enough that a body's own
	// half-extent never bounds the guess before the horizon does. The fast case
	// has a test of its own.
	constexpr float WALKING_METRES_PER_SECOND = 1.0f;

	// Ticks received before anything is measured, and where the last of them
	// puts a body moving at `WALKING_METRES_PER_SECOND`.
	constexpr uint64_t WARM_TICKS = 12;
	constexpr float WARM_METRES =
		WALKING_METRES_PER_SECOND * static_cast<float>(WARM_TICKS) / static_cast<float>(TICK_RATE);
}

TEST_CASE("the horizon is the wire's number and not this module's", "[client][replication]") {
	// **Rule 6.** `replication` links no simulation module and may not see the
	// grid the horizon is derived from, so the number is stated in two places
	// and this is the check the build cannot make. `scene::Wire.hpp` carries the
	// derivation; if it ever moves, this fails rather than a client quietly
	// guessing past the point where the guess is worth having.
	InterpolationSettings defaults;
	CHECK(defaults.ExtrapolateSeconds == static_cast<double>(engine::scene::WIRE_DEAD_RECKON_SECONDS));
}

TEST_CASE("a body nobody owns is dead-reckoned toward the authority", "[client][replication]") {
	Replica replica;
	const Entity entity = replica.SpawnMoving(WALKING_METRES_PER_SECOND);

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}

	// A second of silence. The authority did not stop moving during it.
	constexpr float SILENT_SECONDS = 1.0f;
	replica.DrawFrames(static_cast<int>(SILENT_SECONDS * TICK_RATE * FRAMES_PER_TICK));

	const float frozen = replica.World.Get<Transform>(entity)->Frame.Position.X;
	const float authority = frozen + WALKING_METRES_PER_SECOND * SILENT_SECONDS;
	const float drawn = replica.Drawn();

	// **The measurement this case exists for.** Frozen is where D00010 leaves
	// it; the guess is a quarter of a second of the velocity the server sent,
	// and a quarter of a second of the right answer is closer than none of it.
	CHECK(frozen == Approx(WARM_METRES));
	CHECK(drawn > frozen);
	CHECK(drawn <= authority);
	CHECK(std::abs(drawn - authority) < std::abs(frozen - authority));

	// And nothing it produced reached a row. The audit hashes what a replica
	// holds, so a guess written back would be reported as disagreeing with the
	// authority on every sweep.
	CHECK(replica.World.Get<Transform>(entity)->Frame.Position.X == Approx(WARM_METRES));
}

TEST_CASE("a body somebody owns is not dead-reckoned", "[client][replication]") {
	// **Extrapolate what nobody owns.** Under v0.13 ownership an owned body is
	// simulated by its owner authoritatively, so there is nothing arriving for a
	// guess to be reconciled against - guessing as well simulates it twice, with
	// the wrong one being whichever the local machine happens not to own.
	Replica replica;
	const Entity entity = replica.SpawnMoving(WALKING_METRES_PER_SECOND);

	// A `Player` handle is all this needs to be: the rule is the presence of the
	// component, not who it names.
	replica.World.Set<engine::scene::NetworkOwner>(
		entity, engine::scene::NetworkOwner{replica.World.Create()}
	);

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}
	replica.DrawFrames(static_cast<int>(TICK_RATE * FRAMES_PER_TICK));

	// D00010's answer, unchanged, for the set this decision deliberately does
	// not cover.
	CHECK(replica.Drawn() == Approx(WARM_METRES));
}

TEST_CASE("a body with no velocity is frozen rather than guessed at", "[client][replication]") {
	// The other half of the same test: there is no function to evaluate, so
	// there is nothing to evaluate it with and the freeze stands.
	Replica replica;
	const Entity entity = replica.Spawn();

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}
	replica.DrawFrames(static_cast<int>(TICK_RATE * FRAMES_PER_TICK));

	CHECK_FALSE(replica.World.Has<engine::scene::Motion>(entity));
	CHECK(replica.Drawn() == Approx(WARM_METRES));
}

TEST_CASE("the horizon stops the guess", "[client][replication]") {
	// Past a quarter of a second the integrated error exceeds the error in the
	// pose it was integrated from, so the guess stops growing and the world
	// holds where the guess left it.
	Replica replica;
	const Entity entity = replica.SpawnMoving(WALKING_METRES_PER_SECOND);

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}

	replica.DrawFrames(static_cast<int>(TICK_RATE * FRAMES_PER_TICK));
	const float atOneSecond = replica.Drawn();

	// Four more seconds of silence buy nothing.
	replica.DrawFrames(static_cast<int>(4.0 * TICK_RATE * FRAMES_PER_TICK));
	const float atFiveSeconds = replica.Drawn();

	const float horizonMetres =
		WALKING_METRES_PER_SECOND * static_cast<float>(InterpolationSettings{}.ExtrapolateSeconds);
	CHECK(atOneSecond == Approx(WARM_METRES + horizonMetres).margin(0.01));
	CHECK(atFiveSeconds == Approx(atOneSecond).margin(1e-5));
}

TEST_CASE("a body is never carried further than its own size", "[client][replication]") {
	// **The bound that stands in for the collision nothing here runs.** At a
	// body length the worst an unrun contact can cost is an overlap with
	// something it was already touching; unbounded it is a crate metres inside a
	// wall, which is worse than the freeze it replaced.
	//
	// Twenty metres a second against a half-metre half-extent: the horizon
	// would carry it five metres and this stops it at a half.
	constexpr float FAST_METRES_PER_SECOND = 20.0f;
	Replica replica;
	const Entity entity = replica.SpawnMoving(FAST_METRES_PER_SECOND);

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, FAST_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}
	replica.DrawFrames(static_cast<int>(TICK_RATE * FRAMES_PER_TICK));

	const float frozen = replica.World.Get<Transform>(entity)->Frame.Position.X;
	const float halfExtent = replica.World.Get<Bounds>(entity)->HalfExtent.X;

	CHECK(replica.Drawn() > frozen);
	CHECK(replica.Drawn() == Approx(frozen + halfExtent).margin(1e-4));
}

TEST_CASE("the guess is unwound rather than snapped away", "[client][replication]") {
	// **The correction decision, asserted as the artefact it exists to avoid.**
	// A guess dropped in one frame is a body that moves backwards by however far
	// it had been carried, which is the one thing more visible than the snap.
	// Unwinding at half real time means it keeps moving forward the whole time.
	Replica replica;
	const Entity entity = replica.SpawnMoving(WALKING_METRES_PER_SECOND);

	for (uint64_t tick = 1; tick <= WARM_TICKS; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		replica.DrawFrames(FRAMES_PER_TICK);
	}

	// Five ticks lost - past the two-tick budget and well inside the resync
	// threshold, so what is measured below is the correction and not the jump
	// D00010 already decided for a pause.
	constexpr uint64_t LOST_TICKS = 5;
	replica.DrawFrames(static_cast<int>(LOST_TICKS) * FRAMES_PER_TICK);
	CHECK(replica.Drawn() > WARM_METRES);

	// The stream returns, at the tick the authority actually reached.
	float previous = replica.Drawn();
	float worstStepBack = 0.0f;
	float largestStep = 0.0f;

	for (uint64_t tick = WARM_TICKS + LOST_TICKS; tick <= WARM_TICKS + LOST_TICKS + 40; tick++) {
		replica.ReceiveMoving(tick, entity, WALKING_METRES_PER_SECOND);
		for (int frame = 0; frame < FRAMES_PER_TICK; frame++) {
			replica.Draw();
			const float drawn = replica.Drawn();
			worstStepBack = std::min(worstStepBack, drawn - previous);
			largestStep = std::max(largestStep, drawn - previous);
			previous = drawn;
		}
	}

	// Never backwards, and never faster than the body's own speed - the guess
	// is given back out of the motion rather than on top of it.
	const float steadyStep = WALKING_METRES_PER_SECOND * FRAME_SECONDS;
	CHECK(worstStepBack >= -1e-5f);
	CHECK(largestStep <= steadyStep * 1.2f);

	// And it is genuinely finished, rather than merely small.
	CHECK(replica.World.Resource<SnapshotBuffer>()->DeadReckonSeconds() == 0.0);
}

// --- the derived halves a replica has to derive for itself -------------------

TEST_CASE("a replica resolves the attachments that arrived", "[client][replication]") {
	// **A replica ticks no simulation, so it has to resolve where a host would
	// have.** Until v0.19 `resolve-attachments` was registered by the scripted
	// and presented paths only, and a replica ran neither - so every
	// `Attachment::WorldFrame` in a joined world stayed at the identity for the
	// whole session and `engine::render::CollectLights`, which reads that field to place
	// a lamp parented to an attachment, lit the world origin.
	Replica replica;

	// **Instances rather than bare entities**, because an attachment resolves
	// against the part it is *parented* to and a bare entity has no place in the
	// tree. That is also what arrives: a snapshot carries instances.
	engine::scene::RegisterSceneClasses();

	const Entity post = replica.World.CreateInstance(engine::scene::PartClass(), "Post");
	REQUIRE(post != engine::ecs::NULL_ENTITY);
	replica.World.GetMutable<Transform>(post)->Frame = CFrame(Vector3{7.0f, 0.0f, 0.0f});

	const Entity point = replica.World.CreateInstance(engine::scene::AttachmentClass(), "Top");
	REQUIRE(replica.World.SetParent(point, post));
	replica.World.GetMutable<Attachment>(point)->Frame = CFrame(Vector3{0.0f, 2.0f, 0.0f});

	replica.Draw();

	const Attachment *resolved = replica.World.Get<Attachment>(point);
	REQUIRE(resolved != nullptr);
	CHECK(resolved->WorldFrame.Position.X == Approx(7.0f));
	CHECK(resolved->WorldFrame.Position.Y == Approx(2.0f));
}
