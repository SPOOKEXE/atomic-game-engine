// Covers the replica presentation seam without a GPU.

#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/Play.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/replication/Protocol.hpp>
#include <engine/replication/SnapshotBuffer.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Attachments.hpp>
#include <engine/scene/CameraContinuation.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Wire.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

TEST_SUITE_ID("client.replicated")
TEST_DEPENDS("engine.ecs.scheduler")
TEST_DEPENDS("engine.ecs.store")
TEST_DEPENDS("engine.replication.snapshotbuffer")
TEST_DEPENDS("engine.scene.components")
TEST_DEPENDS("engine.scene.drawinstance")
TEST_DEPENDS("engine.scene.attachments")
TEST_DEPENDS("engine.physics.characters")

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

		Entity SpawnLocalCharacter() {
			const Entity player = World.Create();
			const Entity character = World.Create();
			const Entity root = Spawn();
			const Entity humanoid = World.Create();

			engine::scene::Humanoid control;
			control.RootPart = root;
			control.Grounded = true;
			World.Set(player, engine::scene::PlayerCharacter{character});
			World.Set(character, engine::scene::Character{root, humanoid, player});
			World.Set(humanoid, control);
			World.Set(root, engine::scene::Motion{});
			World.SetResource(engine::scene::LocalPlayer{player});
			return root;
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

TEST_CASE(
	"a local player is reset from authority and replays only unconfirmed moves",
	"[client][replication][prediction]"
) {
	Replica replica;
	const Entity root = replica.SpawnLocalCharacter();
	replica.World.SetFrame(static_cast<float>(1.0 / TICK_RATE), 0.0f);

	engine::game::MoveInput move;
	move.Direction = Vector3{1.0f, 0.0f, 0.0f};
	move.StepSeconds = GENERATE(0.0, 0.01, 0.03);
	const float replayStep =
		move.StepSeconds > 0 ? static_cast<float>(move.StepSeconds) : 1.0f / static_cast<float>(TICK_RATE);
	std::vector<engine::replication::Input> pending;
	pending.push_back({2, engine::game::EncodeMoveInput(move)});

	client::ReconcileLocalPlayerPrediction(replica.World, 1, pending);
	const auto *prediction = replica.World.Resource<client::LocalPlayerPrediction>();
	REQUIRE(prediction != nullptr);
	CHECK(prediction->Active);
	CHECK(prediction->Root == root);
	CHECK(prediction->Frame.Position.X == Approx(16.0f * replayStep));
	CHECK(replica.World.Resource<SnapshotBuffer>()->Predicted() == root);
	CHECK(replica.World.Get<Transform>(root)->Frame.Position.X == 0.0f);

	// The overlay, rather than a transform write into the authority's replica,
	// is what the draw collector sees.
	replica.Draw();
	CHECK(replica.Drawn() == Approx(16.0f * replayStep));

	// A prediction has already integrated its move. The stalled-snapshot
	// dead-reckoner applies only to ordinary replicated bodies, otherwise a
	// still-present authority velocity would advance this overlay again every
	// rendered frame.
	replica.Receive(1, root, 0.0f);
	replica.World.GetMutable<engine::scene::Motion>(root)->Linear = Vector3{60.0f, 0.0f, 0.0f};
	replica.DrawFrames(120);
	CHECK(replica.Drawn() == Approx(16.0f * replayStep));

	// A new authority tick owns the baseline. With no surviving inputs the old
	// local movement disappears instead of accumulating indefinitely.
	replica.World.GetMutable<Transform>(root)->Frame = CFrame(Vector3{4.0f, 0.0f, 0.0f});
	client::ReconcileLocalPlayerPrediction(replica.World, 2, {});
	prediction = replica.World.Resource<client::LocalPlayerPrediction>();
	REQUIRE(prediction != nullptr);
	CHECK(prediction->Frame.Position.X == 4.0f);

	client::PredictLocalPlayerMove(replica.World, move, static_cast<float>(1.0 / TICK_RATE));
	CHECK(prediction->Frame.Position.X == Approx(4.0f + 16.0f / static_cast<float>(TICK_RATE)));
	CHECK(replica.World.Get<Transform>(root)->Frame.Position.X == 4.0f);
}

TEST_CASE(
	"predicted body and humanoid camera share fractional tick movement", "[client][prediction][camera]"
) {
	Replica replica;
	auto &store = replica.World;
	const Entity root = replica.SpawnLocalCharacter();
	const auto player = store.Resource<engine::scene::LocalPlayer>()->Instance;
	const auto model = engine::scene::CharacterOf(store, player);
	const auto rig = *store.Get<engine::scene::Character>(model);
	const auto camera = client::AimReplicaViewer(store, CFrame{}, engine::scene::Camera{});
	const auto limb = replica.Spawn();
	store.Set(limb, engine::scene::CharacterLimb{.Root = root, .Offset = CFrame(Vector3{0, 2, 0})});
	store.Set(limb, Transform{CFrame(Vector3{0, 2, 0})});
	const int selection = GENERATE(0, 1, 2);
	const auto target = selection == 0 ? rig.Humanoid : selection == 1 ? root : limb;
	store.Set(camera, engine::scene::CameraSubject{.Target = target, .Automatic = false});
	client::ReconcileLocalPlayerPrediction(store, 1, {});
	const float tickSeconds = GENERATE(1.0f / 30, 1.0f / 60, 1.0f / 120);
	store.AdvanceTick(tickSeconds);
	auto *prediction = store.ResourceMutable<client::LocalPlayerPrediction>();
	prediction->Linear = {GENERATE(-16.0f, 0.0f, 16.0f), 0, 0};
	const auto baseline = prediction->Frame;
	const auto authority = store.Get<Transform>(root)->Frame;
	store.SetFrame(tickSeconds / 4, .25f);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const float bodyQuarter = replica.Drawn();
	const auto eyeQuarter = store.Get<Transform>(camera)->Frame.Position;
	store.SetFrame(tickSeconds / 2, .75f);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const float expectedStep = prediction->Linear.X * tickSeconds * .5f;
	CHECK(replica.Drawn() - bodyQuarter == Approx(expectedStep).margin(.00001));
	CHECK(
		store.Get<Transform>(camera)->Frame.Position.X - eyeQuarter.X == Approx(expectedStep).margin(.00001)
	);
	CHECK(prediction->Frame.Position == baseline.Position);
	CHECK(prediction->AuthorityTick == 1);
	CHECK(store.Get<Transform>(root)->Frame.Position == authority.Position);
}

TEST_CASE("prediction correction shares one bounded body and camera pose", "[client][prediction][camera]") {
	Replica replica;
	auto &store = replica.World;
	CHECK_FALSE(client::PresentedPlayerPrediction(store));
	const Entity root = replica.SpawnLocalCharacter();
	const auto player = store.Resource<engine::scene::LocalPlayer>()->Instance;
	const auto rig = *store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, player));
	const auto camera = client::AimReplicaViewer(store, CFrame{}, engine::scene::Camera{});
	store.Set(camera, engine::scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
	constexpr float step = 1.f / 60;
	client::ReconcileLocalPlayerPrediction(store, 1, {});
	const int initialSteps = GENERATE(1, 6);
	for (int tick = 0; tick < initialSteps; ++tick)
		client::PredictLocalPlayerMove(store, {{1, 0, 0}, false}, step);
	store.SetFrame(step, 0);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const float before = replica.Drawn();
	const float eye = store.Get<Transform>(camera)->Frame.Position.X;
	client::ReconcileLocalPlayerPrediction(store, 2, {});
	CHECK(store.Resource<client::LocalPlayerPrediction>()->Frame.Position.X == Approx(0));
	CHECK(client::PresentedPlayerPrediction(store)->Position.X == Approx(before));
	client::PredictLocalPlayerMove(store, {{1, 0, 0}, false}, step);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const float movement = replica.Drawn() - before;
	CHECK(movement >= 16 * step * .49f);
	CHECK(store.Get<Transform>(camera)->Frame.Position.X - eye == Approx(movement));
	CHECK(replica.Drawn() == Approx(client::PresentedPlayerPrediction(store)->Position.X));
	CHECK(store.Get<Transform>(root)->Frame.Position.X == Approx(0));

	const float priorCorrection = client::PresentedPlayerPrediction(store)->Position.X;
	store.Set(root, Transform{CFrame(Vector3{-.1f, 0, 0})});
	client::ReconcileLocalPlayerPrediction(store, 3, {});
	CHECK(client::PresentedPlayerPrediction(store)->Position.X == Approx(priorCorrection));
	client::PredictLocalPlayerMove(store, {{}, false}, step);
	store.SetFrame(store.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds * .5f, 0);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const float remaining = store.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds;
	client::ReconcileLocalPlayerPrediction(store, 4, {});
	CHECK(store.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds == remaining);
	store.SetFrame(remaining + .01f, 0);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	CHECK(replica.Drawn() == Approx(-.1f));
	CHECK(store.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds == 0);
	CHECK(store.Resource<client::LocalPlayerPrediction>()->PositionCorrection == Vector3::Zero);
	CHECK(store.Resource<client::LocalPlayerPrediction>()->AuthorityTick == 4);
	store.ResourceMutable<client::LocalPlayerPrediction>()->Frame.Position.X = 1000;
	client::ReconcileLocalPlayerPrediction(store, 5, {});
	CHECK(client::PresentedPlayerPrediction(store)->Position.X == Approx(-.1f));
	CHECK(store.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds == 0);
}

TEST_CASE(
	"local prediction respects scripted and unrelated camera subjects", "[client][prediction][camera]"
) {
	Replica replica;
	auto &store = replica.World;
	const auto root = replica.SpawnLocalCharacter();
	const auto player = store.Resource<engine::scene::LocalPlayer>()->Instance;
	const auto rig = *store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, player));
	const auto other = replica.Spawn();
	store.Set(other, Transform{CFrame(Vector3{20, 0, 0})});
	const auto camera = client::AimReplicaViewer(store, CFrame(Vector3{10, 15, 25}), engine::scene::Camera{});
	const int selection = GENERATE(0, 1, 2, 3);
	INFO("camera selection " << selection);
	const auto target = selection == 0 ? rig.Humanoid : selection == 2 ? other : engine::ecs::NULL_ENTITY;
	store.Set(camera, engine::scene::CameraSubject{.Target = target, .Automatic = selection == 3});
	if (selection == 0 || selection == 3)
		store.ResourceMutable<engine::scene::CameraController>()->Mode =
			engine::scene::CameraMode::Scriptable;
	client::ReconcileLocalPlayerPrediction(store, 1, {});
	auto *prediction = store.ResourceMutable<client::LocalPlayerPrediction>();
	prediction->Frame = CFrame(Vector3{4, 0, 0});
	prediction->Linear = {16, 0, 0};
	prediction->Active = false;
	store.SetFrame(FRAME_SECONDS, .75f);
	replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
	const auto expected = store.Get<Transform>(camera)->Frame;
	prediction->Active = true;
	for (int frame = 0; frame < 2; ++frame) {
		engine::scene::Camera fallbackLens;
		fallbackLens.FarPlane = 100;
		CHECK(client::AimReplicaViewer(store, CFrame(Vector3{99, 99, 99}), fallbackLens) == camera);
		CHECK(store.Get<engine::scene::Camera>(camera)->FarPlane == engine::scene::Camera{}.FarPlane);
		replica.Systems.RunPhases(store, Phase::PreRender, Phase::PreRender);
		CHECK(store.Get<Transform>(camera)->Frame.Position == expected.Position);
		CHECK(store.Get<engine::scene::CameraSubject>(camera)->Target == target);
	}
	CHECK(store.Get<Transform>(root)->Frame.Position.X == 0);
}

TEST_CASE("a replica keeps its third-person camera inside received walls", "[client][replication][camera]") {
	// The authority sends the wall and the character, but camera placement is
	// local presentation. A replica therefore needs a query index of its own for
	// poppercam even though it never advances physics. Without that index and
	// pass, a small orbit inside a corridor puts the eye through its wall while
	// the character continues to render correctly.
	Replica replica;

	const Entity subject = replica.Spawn();
	replica.World.GetMutable<Transform>(subject)->Frame = CFrame(Vector3{0.0f, 0.0f, 0.0f});

	const Entity wall = replica.Spawn();
	replica.World.GetMutable<Transform>(wall)->Frame = CFrame(Vector3{0.0f, 1.5f, 6.0f});
	replica.World.GetMutable<Bounds>(wall)->HalfExtent = Vector3{5.0f, 5.0f, 0.25f};
	engine::scene::Collider wallCollider;
	wallCollider.Extent = Vector3{5.0f, 5.0f, 0.25f};
	replica.World.Set(wall, wallCollider);

	const Entity camera = client::AimReplicaViewer(replica.World, CFrame{}, engine::scene::Camera{});
	REQUIRE(camera != engine::ecs::NULL_ENTITY);
	CHECK(replica.World.ClassOf(camera) == engine::ecs::Classes::Find(engine::core::Name("Camera")));

	auto *controller = replica.World.ResourceMutable<engine::scene::CameraController>();
	REQUIRE(controller != nullptr);
	replica.World.Set(camera, engine::scene::CameraSubject{.Target = subject, .Automatic = false});
	controller->Angles = engine::core::Vector2{0.0f, 0.0f};
	controller->Distance = 12.0f;

	client::RecordReplicatedTick(replica.World, 1);
	replica.Draw();

	controller = replica.World.ResourceMutable<engine::scene::CameraController>();
	REQUIRE(controller != nullptr);
	REQUIRE(controller->OccludedDistance >= 0.0f);
	const auto *placed = replica.World.Get<Transform>(camera);
	REQUIRE(placed != nullptr);
	CHECK(placed->Frame.Position.Z < 5.75f);
	CHECK(placed->Frame.Position.Z > 0.0f);
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

TEST_CASE(
	"prediction replays unconfirmed input from a younger client clock", "[client][replication][prediction]"
) {
	Replica replica;
	const Entity root = replica.SpawnLocalCharacter();
	engine::game::MoveInput move;
	move.Direction = Vector3{1, 0, 0};
	const std::vector<engine::replication::Input> pending{{2, engine::game::EncodeMoveInput(move)}};
	client::ReconcileLocalPlayerPrediction(replica.World, 900, pending);
	const auto *prediction = replica.World.Resource<client::LocalPlayerPrediction>();
	REQUIRE(prediction != nullptr);
	CHECK(prediction->Root == root);
	CHECK(prediction->Frame.Position.X == Approx(16.0f / static_cast<float>(TICK_RATE)));
}

TEST_CASE(
	"retained character prediction advances across unrelated authority updates",
	"[client][replication][prediction][camera-hold]"
) {
	namespace scene = engine::scene;
	Replica replica;
	auto &store = replica.World;
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto rig = *store.Get<scene::Character>(model);
	const auto camera = store.CreatePredictedInstance(scene::CameraClass(), "local camera");
	store.Set(camera, scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
	store.SetResource(scene::ActiveCamera{camera});
	store.SetResource(scene::LocalPlayer{player});
	store.SetAdoptOnly(true);
	client::ReconcileLocalPlayerPrediction(store, 100, {});
	auto prediction = *store.Resource<client::LocalPlayerPrediction>();
	REQUIRE(prediction.Active);
	REQUIRE(scene::PrepareCameraCharacterHold(store, player, prediction.Frame));
	store.DestroyInstance(rig.Root);
	REQUIRE(scene::ActivateCameraCharacterHold(store));
	const auto held = *store.Resource<scene::CameraCharacterHold>();
	prediction.Player = held.Player;
	prediction.Root = held.Root;
	store.SetResource(prediction);
	const float start = prediction.Frame.Position.X;
	engine::game::MoveInput move;
	move.Direction = Vector3{1, 0, 0};
	for (uint64_t tick = 101; tick < 111; ++tick) {
		client::PredictLocalPlayerMove(store, move, 1.0f / 60.0f);
		client::ReconcileLocalPlayerPrediction(store, tick, {});
		const auto &current = *store.Resource<client::LocalPlayerPrediction>();
		CHECK(current.Active);
		CHECK(current.Root == held.Root);
		CHECK(
			current.Frame.Position.X == Approx(start + (tick - 100) * prediction.Humanoid.WalkSpeed / 60.0f)
		);
		CHECK(store.Get<Transform>(held.Root)->Frame.Position.X == start);
	}
	scene::ReleaseCameraCharacterHold(store);
	client::ReconcileLocalPlayerPrediction(store, 111, {});
	CHECK_FALSE(store.Resource<client::LocalPlayerPrediction>()->Active);
}

TEST_CASE(
	"portal replay retains submitted durations and maps through a scaled seam",
	"[client][prediction][portal-input-history]"
) {
	namespace scene = engine::scene;
	Replica replica;
	auto &store = replica.World;
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto rig = *store.Get<scene::Character>(model);
	const auto camera = store.CreatePredictedInstance(scene::CameraClass(), "camera");
	store.Set(camera, scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
	store.SetResource(scene::ActiveCamera{camera});
	store.SetResource(scene::LocalPlayer{player});
	store.SetAdoptOnly(true);
	client::ReconcileLocalPlayerPrediction(store, 100, {});
	auto prediction = *store.Resource<client::LocalPlayerPrediction>();
	REQUIRE(scene::PrepareCameraCharacterHold(store, player, prediction.Frame));
	store.DestroyInstance(rig.Root);
	REQUIRE(scene::ActivateCameraCharacterHold(store));
	const auto held = *store.Resource<scene::CameraCharacterHold>();
	prediction.Player = held.Player;
	prediction.Root = held.Root;
	store.SetResource(prediction);
	engine::game::PortalResume claim;
	claim.Transfer = {"source", 1, 1};
	claim.Destination = "destination";
	claim.DestinationIncarnation = 2;
	claim.SourceSession = 3;
	claim.Capability.fill(std::byte{42});
	const scene::SeamTransform through{CFrame(Vector3{10, 20, 30}) * CFrame::Angles(0, .7f, 0), {2, 3, 4}, 2};
	REQUIRE(client::BeginPortalInputHistory(store, claim, through, 100));
	REQUIRE(client::RecordPortalPredictionInput(store, 101, {{1, 0, 0}, true}, .01f));
	REQUIRE(client::RecordPortalPredictionInput(store, 102, {{0, 0, -1}, false}, .03f));
	CHECK_FALSE(client::RecordPortalPredictionInput(store, 102, {{}, true}, .1f));
	client::ReconcileLocalPlayerPrediction(store, 40000, {});
	CHECK(store.Resource<client::PortalInputHistory>()->Count == 2);
	engine::script::PortalTransferMotion motion;
	motion.DestinationIncarnation = 2;
	motion.DestinationTick = 50;
	motion.InputTick = 100;
	motion.Frame = through.Place(prediction.Frame);
	motion.WalkSpeed = 32;
	motion.JumpSpeed = 14;
	motion.Grounded = true;
	REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
	const auto &replayed = *store.Resource<client::LocalPlayerPrediction>();
	const auto x = through.Rotate({1, 0, 0});
	const auto z = through.Rotate({0, 0, -1});
	const auto expected = motion.Frame.Position + Vector3{x.X, 0, x.Z} * .32f + Vector3{z.X, 0, z.Z} * .96f +
						  Vector3{0, .56f, 0};
	const auto mapped = through.Place(replayed.Frame);
	CHECK(mapped.Position.X == Approx(expected.X));
	CHECK(mapped.Position.Y == Approx(expected.Y));
	CHECK(mapped.Position.Z == Approx(expected.Z));
	CHECK(replayed.Humanoid.WalkSpeed == Approx(16));
	CHECK(replayed.Humanoid.JumpSpeed == Approx(7));
	CHECK(replayed.Humanoid.MoveDirection.X == Approx(0).margin(.00001));
	CHECK(replayed.Humanoid.MoveDirection.Z == Approx(-1));
	CHECK_FALSE(replayed.Humanoid.Grounded);
	CHECK(store.Get<Transform>(held.Root)->Frame.Position == prediction.Frame.Position);
	CHECK_FALSE(client::ReconcilePortalInputHistory(store, claim, motion));
	CHECK(store.Resource<client::PortalInputHistory>()->Count == 2);
	const auto carried = client::CapturePortalPrediction(store, claim);
	REQUIRE(carried);
	REQUIRE(carried->Inputs.size() == 2);
	CHECK(carried->CoveredThrough == motion.InputTick);
	const auto correction = through.Carry(replayed.PositionCorrection);
	CHECK(carried->PositionCorrection.X == Approx(correction.X));
	CHECK(carried->PositionCorrection.Y == Approx(correction.Y));
	CHECK(carried->PositionCorrection.Z == Approx(correction.Z));
	CHECK(carried->CorrectionSeconds == replayed.CorrectionSeconds);
	for (size_t index = 0; index < carried->Inputs.size(); ++index) {
		const auto *history = store.Resource<client::PortalInputHistory>();
		const auto &source = history->Inputs[(history->Begin + index) % client::PortalInputHistory::CAPACITY];
		engine::game::MoveInput decoded;
		REQUIRE(engine::game::DecodeMoveInput(carried->Inputs[index].Bytes, decoded));
		CHECK(carried->Inputs[index].Tick == source.Tick);
		CHECK(decoded.StepSeconds == source.Delta);
		CHECK(decoded.Jump == source.Move.Jump);
		const auto mappedMove = through.Rotate(source.Move.Direction);
		CHECK(decoded.Direction.X == Approx(mappedMove.X));
		CHECK(decoded.Direction.Z == Approx(mappedMove.Z));
	}

	motion.DestinationTick = 51;
	motion.InputTick = 102;
	motion.Frame = mapped;
	motion.Grounded = false;
	REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
	CHECK(
		store.Resource<client::LocalPlayerPrediction>()->Humanoid.MoveDirection.X == Approx(0).margin(.00001)
	);
	CHECK(store.Resource<client::LocalPlayerPrediction>()->Humanoid.MoveDirection.Z == Approx(-1));
	CHECK(store.Resource<client::PortalInputHistory>()->Count == 0);
	bool recorded = true;
	const float sourceAlpha = .75f;
	const auto continuation = client::CapturePortalPrediction(store, claim, sourceAlpha);
	REQUIRE(continuation);
	CHECK(continuation->Motion.Frame.Position.X == Approx(mapped.Position.X));
	CHECK(continuation->Motion.WalkSpeed == Approx(32));
	CHECK(continuation->MoveDirection.X == Approx(z.X));
	Replica destination;
	auto &arrived = destination.World;
	scene::InstallServices(arrived);
	const auto arrivedPlayer = scene::AddPlayer(arrived, "arrived", false, 92);
	const auto arrivedModel = scene::LoadCharacter(arrived, arrivedPlayer);
	const auto arrivedRig = *arrived.Get<scene::Character>(arrivedModel);
	const auto authorityFrame = arrived.Get<Transform>(arrivedRig.Root)->Frame;
	arrived.SetResource(scene::LocalPlayer{arrivedPlayer});
	arrived.SetAdoptOnly(true);
	arrived.AdvanceTick(.02f);
	const float destinationAlpha = .25f;
	REQUIRE(client::AdoptPortalPrediction(arrived, arrivedPlayer, *continuation, destinationAlpha));
	const double phaseOffset = sourceAlpha * static_cast<double>(store.Time().Delta) -
							   destinationAlpha * static_cast<double>(arrived.Time().Delta);
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->PresentationOffsetSeconds == Approx(phaseOffset)
	);

	const auto &adopted = *arrived.Resource<client::LocalPlayerPrediction>();
	CHECK(adopted.Player == arrivedPlayer);
	CHECK(adopted.Root == arrivedRig.Root);
	CHECK(adopted.Humanoid.RootPart == arrived.Get<scene::Humanoid>(arrivedRig.Humanoid)->RootPart);
	CHECK(adopted.Frame.Position.X == Approx(mapped.Position.X));
	const auto drawnRoot = [](Replica &replica, Entity root, float alpha) {
		replica.World.Set(root, Bounds{Vector3{.5f, .5f, .5f}});
		replica.World.Set(root, Visual{});
		replica.World.SetFrame(0, alpha);
		replica.Systems.RunPhases(replica.World, Phase::PreRender, Phase::PreRender);
		const auto &instances = replica.Instances();
		const auto found = std::find_if(instances.begin(), instances.end(), [root](const auto &instance) {
			return instance.Source == root.Id;
		});
		REQUIRE(found != instances.end());
		return found->Frame;
	};
	const auto sourcePresented = through.Place(drawnRoot(replica, held.Root, sourceAlpha));
	const auto arrivedPresented = drawnRoot(destination, arrivedRig.Root, destinationAlpha);
	CHECK(arrivedPresented.Position.X == Approx(sourcePresented.Position.X).margin(.00001));
	CHECK(arrivedPresented.Position.Y == Approx(sourcePresented.Position.Y).margin(.00001));
	CHECK(arrivedPresented.Position.Z == Approx(sourcePresented.Position.Z).margin(.00001));

	CHECK(arrived.Get<Transform>(arrivedRig.Root)->Frame.Position == authorityFrame.Position);
	client::ReconcileLocalPlayerPrediction(arrived, 50, {});
	CHECK(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.X == Approx(mapped.Position.X));
	client::ReconcileLocalPlayerPrediction(arrived, 52, {});
	CHECK(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position == authorityFrame.Position);
	CHECK_FALSE(client::AdoptPortalPrediction(arrived, engine::ecs::NULL_ENTITY, *continuation));
	engine::game::PlayerMotion native{arrivedPlayer, arrivedRig.Root, continuation->Motion};
	native.Motion.DestinationTick = 53;
	native.Motion.InputTick = 2;
	native.Motion.Frame = CFrame(Vector3{40, 20, 10});
	native.Motion.WalkSpeed = 16;
	CHECK_FALSE(client::AcceptNativePlayerMotion(arrived, native, 1));
	REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 3));
	const double inputStep = GENERATE(0.0, 0.01, 0.03);
	const float replayStep = inputStep > 0 ? static_cast<float>(inputStep) : arrived.Time().Delta;
	const std::array<engine::replication::Input, 2> pending{
		{{2, engine::game::EncodeMoveInput({{0, 0, -1}, false, inputStep})},
		 {3, engine::game::EncodeMoveInput({{1, 0, 0}, false, inputStep})}}
	};
	CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, pending, 3));
	const auto acknowledged = client::ReconcileNativePlayerPrediction(arrived, pending, 2);
	REQUIRE(acknowledged);
	CHECK(*acknowledged == 2);
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->PresentationOffsetSeconds == Approx(phaseOffset)
	);
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.X == Approx(40 + 16 * replayStep)
	);
	CHECK(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.Z == Approx(10));
	CHECK(arrived.Get<Transform>(arrivedRig.Root)->Frame.Position == authorityFrame.Position);
	CHECK_FALSE(client::AcceptNativePlayerMotion(arrived, native, 3));
	CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, pending, 2));
	native.Motion.DestinationTick++;
	native.Motion.InputTick = 1;
	CHECK_FALSE(client::AcceptNativePlayerMotion(arrived, native, 3));
	native.Motion.InputTick = 3;
	native.Motion.DestinationIncarnation++;
	CHECK_FALSE(client::AcceptNativePlayerMotion(arrived, native, 3));
	--native.Motion.DestinationIncarnation;
	arrived.Remove<Transform>(arrivedRig.Root);
	REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 3));
	CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, pending, 2));
	CHECK(arrived.Resource<client::NativePlayerPrediction>()->AppliedPoseTick == 53);
	arrived.Set(arrivedRig.Root, Transform{authorityFrame});
	CHECK(client::ReconcileNativePlayerPrediction(arrived, pending, 2) == 3);
	native.Motion.DestinationTick++;
	native.Motion.Frame.Position.Y = std::numeric_limits<float>::max();
	native.Motion.Linear.Y = std::numeric_limits<float>::max();
	REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 4));
	const std::array<engine::replication::Input, 1> overflowInput{
		{{4, engine::game::EncodeMoveInput({{1, 0, 0}, false})}}
	};
	CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, overflowInput, 3));
	CHECK(arrived.Resource<client::NativePlayerPrediction>()->AppliedPoseTick == 54);
	CHECK(std::isfinite(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.Y));
	++native.Motion.DestinationTick;
	native.Motion.Frame.Position.Y = 0;
	native.Motion.Linear.Y = 0;
	REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 4));
	const std::array<engine::replication::Input, 1> overflowDuration{
		{{4, engine::game::EncodeMoveInput({{1, 0, 0}, false, std::numeric_limits<double>::max()})}}
	};
	CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, overflowDuration, 3));
	CHECK(arrived.Resource<client::NativePlayerPrediction>()->AppliedPoseTick == 54);
	for (uint64_t tick = 103; tick <= 1127; ++tick)
		recorded &= client::RecordPortalPredictionInput(store, tick, {{1, 0, 0}, false}, .01f);
	REQUIRE(recorded);
	const auto *history = store.Resource<client::PortalInputHistory>();
	CHECK(history->Count == client::PortalInputHistory::CAPACITY);
	CHECK(history->DiscardedInputs == 1);
	CHECK(history->CoveredThrough == 103);
	motion.DestinationTick = 52;
	CHECK_FALSE(client::ReconcilePortalInputHistory(store, claim, motion));
	motion.InputTick = 1127;
	auto wrong = claim;
	wrong.Capability[0] ^= std::byte{1};
	CHECK_FALSE(client::ReconcilePortalInputHistory(store, wrong, motion));
	REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
	CHECK(store.Resource<client::PortalInputHistory>()->Count == 0);
	Store authority{"portal-history-authority"};
	scene::InstallServices(authority);
	engine::core::ByteWriter replacement;
	REQUIRE(authority.Save(replacement));
	engine::core::ByteReader replacing(replacement.Bytes());
	REQUIRE(store.Apply(replacing, engine::ecs::ApplyMode::Authoritative));
	REQUIRE(store.Resource<client::PortalInputHistory>());
	CHECK(store.Resource<client::PortalInputHistory>()->Claim == claim);
	CHECK(store.Resource<client::PortalInputHistory>()->LastRecordedTick == 1127);
	engine::core::ByteWriter saved;
	REQUIRE(store.Save(saved));
	Store restored{"portal-history-restored"};
	engine::core::ByteReader reader(saved.Bytes());
	REQUIRE(restored.Apply(reader, engine::ecs::ApplyMode::Authoritative));
	REQUIRE(restored.Resource<client::PortalInputHistory>());
	CHECK(restored.Resource<client::PortalInputHistory>()->Claim.Destination.empty());
	CHECK(restored.Resource<client::PortalInputHistory>()->LastRecordedTick == 0);
	CHECK_FALSE(client::RecordPortalPredictionInput(restored, 1, {{1, 0, 0}, false}, .01f));
}

TEST_CASE(
	"portal replay keeps simulation time through coalesced input and adoption",
	"[client][prediction][portal-input-history]"
) {
	namespace scene = engine::scene;
	Replica replica;
	auto &store = replica.World;
	scene::InstallServices(store);
	const auto player = scene::AddPlayer(store, "viewer", false, 91);
	const auto model = scene::LoadCharacter(store, player);
	const auto rig = *store.Get<scene::Character>(model);
	const auto camera = store.CreatePredictedInstance(scene::CameraClass(), "camera");
	store.Set(camera, scene::CameraSubject{.Target = rig.Humanoid, .Automatic = false});
	store.SetResource(scene::ActiveCamera{camera});
	store.SetResource(scene::LocalPlayer{player});
	store.SetAdoptOnly(true);
	client::ReconcileLocalPlayerPrediction(store, 100, {});
	auto prediction = *store.Resource<client::LocalPlayerPrediction>();
	REQUIRE(scene::PrepareCameraCharacterHold(store, player, prediction.Frame));
	store.DestroyInstance(rig.Root);
	REQUIRE(scene::ActivateCameraCharacterHold(store));
	const auto held = *store.Resource<scene::CameraCharacterHold>();
	prediction.Player = held.Player;
	prediction.Root = held.Root;
	store.SetResource(prediction);
	engine::game::PortalResume claim;
	claim.Transfer = {"source", 1, 1};
	claim.Destination = "destination";
	claim.DestinationIncarnation = 2;
	claim.SourceSession = 3;
	claim.Capability.fill(std::byte{42});
	REQUIRE(client::BeginPortalInputHistory(store, claim, {}, 100));
	constexpr float inputStep = 1.0f / 60;
	const engine::game::MoveInput move{{1, 0, 0}, false, inputStep};
	for (uint64_t tick = 101; tick <= 104; ++tick)
		REQUIRE(client::RecordPortalPredictionInput(store, tick, move, inputStep));
	engine::script::PortalTransferMotion motion;
	motion.DestinationIncarnation = 2;
	motion.DestinationTick = 50;
	motion.InputTick = 100;
	motion.Linear = {16, 0, 0};
	motion.WalkSpeed = 16;
	motion.JumpSpeed = 7;
	motion.Grounded = true;
	motion.SimulationSeconds = 1;
	REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
	const float before = store.Resource<client::LocalPlayerPrediction>()->Frame.Position.X;
	CHECK(before == Approx(16 * inputStep * 4));
	REQUIRE(client::RecordPortalPredictionInput(store, 105, move, inputStep));
	motion.DestinationTick++;
	motion.InputTick = 103;
	motion.SimulationSeconds += inputStep * 2;
	motion.Frame.Position.X = 16 * inputStep * 2;
	REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
	const float after = store.Resource<client::LocalPlayerPrediction>()->Frame.Position.X;
	CHECK(after - before == Approx(16 * inputStep));
	CHECK(store.Resource<client::PortalInputHistory>()->CoveredThrough == 103);
	CHECK(store.Resource<client::PortalInputHistory>()->Clock.InputLeadSeconds == Approx(inputStep));

	const auto continuation = client::CapturePortalPrediction(store, claim);
	REQUIRE(continuation);
	Replica destination;
	auto &arrived = destination.World;
	scene::InstallServices(arrived);
	const auto arrivedPlayer = scene::AddPlayer(arrived, "arrived", false, 92);
	const auto arrivedModel = scene::LoadCharacter(arrived, arrivedPlayer);
	const auto arrivedRig = *arrived.Get<scene::Character>(arrivedModel);
	arrived.SetResource(scene::LocalPlayer{arrivedPlayer});
	arrived.SetAdoptOnly(true);
	REQUIRE(client::AdoptPortalPrediction(arrived, arrivedPlayer, *continuation));
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->PositionCorrection ==
		continuation->PositionCorrection
	);
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->CorrectionSeconds ==
		continuation->CorrectionSeconds
	);
	engine::game::PlayerMotion native{arrivedPlayer, arrivedRig.Root, motion};
	native.Motion.DestinationTick++;
	native.Motion.InputTick = 105;
	native.Motion.SimulationSeconds += inputStep * 2;
	native.Motion.Frame.Position.X += 16 * inputStep * 2;
	auto inputs = continuation->Inputs;
	inputs.push_back({106, engine::game::EncodeMoveInput(move)});
	REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 106));
	REQUIRE(client::ReconcileNativePlayerPrediction(arrived, inputs, 103) == 105);
	CHECK(
		arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.X - after == Approx(16 * inputStep)
	);

	SECTION("completed time overtakes input acknowledgement") {
		native.Motion.DestinationTick += 2;
		native.Motion.SimulationSeconds += inputStep * 4;
		native.Motion.Frame.Position.X += 16 * inputStep * 4;
		REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 106));
		for (uint64_t tick = 107; tick <= 109; ++tick) {
			auto pendingMove = move;
			pendingMove.Jump = tick == 107;
			inputs.push_back({tick, engine::game::EncodeMoveInput(pendingMove)});
		}
		REQUIRE(client::ReconcileNativePlayerPrediction(arrived, inputs, 105) == 105);
		CHECK_FALSE(arrived.Resource<client::LocalPlayerPrediction>()->Humanoid.Grounded);
		CHECK(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.Y == Approx(7 * inputStep));
		CHECK(
			arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position.X == Approx(16 * inputStep * 9)
		);
	}
	SECTION("slow client accepts authority beyond its entire input horizon") {
		for (uint64_t tick = 106; tick <= 108; ++tick) {
			native.Motion.DestinationTick += 4;
			native.Motion.SimulationSeconds += inputStep * 4;
			native.Motion.InputTick = tick;
			native.Motion.Frame.Position.X += 16 * inputStep * 4;
			auto pendingMove = move;
			pendingMove.Jump = true;
			inputs.push_back({tick + 1, engine::game::EncodeMoveInput(pendingMove)});
			REQUIRE(client::AcceptNativePlayerMotion(arrived, native, tick + 1));
			REQUIRE(client::ReconcileNativePlayerPrediction(arrived, inputs, tick - 1) == tick);
			const auto *replayed = arrived.Resource<client::LocalPlayerPrediction>();
			CHECK(replayed->Frame.Position == native.Motion.Frame.Position);
			CHECK_FALSE(replayed->Humanoid.Grounded);
			CHECK(replayed->Linear.Y == Approx(7));
			const auto *applied = arrived.Resource<client::NativePlayerPrediction>();
			CHECK(applied->AppliedPoseTick == native.Motion.DestinationTick);
			CHECK(applied->Clock.InputLeadSeconds == Approx(-inputStep));
		}
	}
	SECTION("held authority advances beyond the recorded input horizon") {
		motion.DestinationTick += 4;
		motion.SimulationSeconds += inputStep * 4;
		motion.InputTick = 105;
		motion.Frame.Position.X += 16 * inputStep * 4;
		REQUIRE(client::ReconcilePortalInputHistory(store, claim, motion));
		CHECK(store.Resource<client::LocalPlayerPrediction>()->Frame.Position == motion.Frame.Position);
		CHECK(store.Resource<client::PortalInputHistory>()->CoveredThrough == 105);
		CHECK(store.Resource<client::PortalInputHistory>()->Clock.InputLeadSeconds == Approx(0));
	}
	SECTION("stale simulation clock does not replace prediction") {
		native.Motion.DestinationTick++;
		REQUIRE(client::AcceptNativePlayerMotion(arrived, native, 106));
		const auto previous = arrived.Resource<client::LocalPlayerPrediction>()->Frame;
		CHECK_FALSE(client::ReconcileNativePlayerPrediction(arrived, inputs, 105));
		CHECK(arrived.Resource<client::LocalPlayerPrediction>()->Frame.Position == previous.Position);
	}
}
