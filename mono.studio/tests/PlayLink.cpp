// The client half of a Play run, which is the one part of it a test can reach.
//
// **`PlayLink` was built precisely so this file could exist.** Everything else
// about Play needs a window, a device and an imgui frame — `Widgets.cpp` says so
// at the top and `mono.studio/AGENTS.md` carries the invariants a test cannot.
// What a viewport shows is not testable here; whether there is anything correct
// *to* show is, and that is the half that can be silently wrong.
//
// The question every case below asks in a different way: **does state actually
// cross?** A client view that draws the server's own rows would pass any test
// about pixels and would be worthless, because the whole value of the panel is
// the difference between the two sides.

#include <engine/ecs/Classes.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/physics/Characters.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Gravity.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Ownership.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <optional>
#include <utility>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/PlayLink.hpp>

TEST_SUITE_ID("studio.playlink")

using engine::core::CFrame;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::scene::Bounds;
using engine::scene::Transform;
using engine::scene::Visual;
using engine::world::Universe;
using engine::world::WorldId;
using engine::world::WorldSettings;
using studio::PlayLink;

namespace {
	constexpr double TICK_RATE = 60.0;
	constexpr float FRAME_SECONDS = 1.0f / static_cast<float>(TICK_RATE);

	// How far a position may move by crossing, and it is not a fudge factor.
	//
	// **A pose is quantised on the wire and this is the documented bound**:
	// `D00015` (a) puts position on a fixed-point grid of +-64 m in 32767 steps
	// each way, which is 0.977 mm per axis anywhere in the world. Asserting
	// equality here was the first thing this file got wrong, and it failed with
	// 1.000030518 against 1.0 — which is the wire working exactly as specified.
	//
	// Stated as the engine's own bound rather than as whatever the run happened
	// to produce: a tolerance fitted to an observed error stops being a check
	// the moment the grid changes.
	constexpr float WIRE_MILLIMETRES = 0.002f;

	// A universe with one authored world in it, as the editor would have.
	struct Fixture {
		Universe Worlds;
		WorldId Authority;

		Fixture() {
			engine::parallel::Jobs::Start(1);
			engine::scene::RegisterSceneComponents();

			WorldSettings settings;
			settings.Name = Name("Scene");
			settings.TickRate = TICK_RATE;
			Authority = Worlds.Create(settings);

			// The dirty bits a delta is built from. Without these the
			// `Observed` half of the replication table finds nothing — silently
			// and for ever, which is exactly the failure `ChangeDetection`
			// documents.
			//
			// **Stated here because this fixture builds no physics, and in a real
			// world it is physics that states it**: `physics::PreparePhysicsWorld`
			// observes `scene::Transform`, and `Editor::BuildWorld` calls it for
			// every world. So this is the fixture standing in for the pipeline it
			// does not install, rather than a switch production forgot —
			// `scene::Motion` is the one that genuinely has no observer outside
			// `mono.server`, and the walking case below is what would notice.
			Worlds.Enter(Authority, [](Store &store) {
				store.Observe<Transform>();
				store.Observe<engine::scene::Motion>();
			});
		}

		~Fixture() {
			engine::parallel::Jobs::Stop();
		}

		Fixture(const Fixture &) = delete;
		Fixture &operator=(const Fixture &) = delete;

		// One drawable row on the authority, at `x`.
		Entity Spawn(float x) {
			Entity entity;
			Worlds.Enter(Authority, [&entity, x](Store &store) {
				entity = store.Create();
				store.Set<Transform>(entity, Transform{CFrame(Vector3{x, 0.0f, 0.0f})});
				store.Set<Bounds>(entity, Bounds{Vector3{0.5f, 0.5f, 0.5f}});
				store.Set<Visual>(entity, Visual{});
			});
			return entity;
		}

		// A step of the link followed by a tick of the universe, which is the
		// order `Editor::Simulate` uses — and the order is the whole of why the
		// "written after the join" case below exists.
		//
		// A world clears its change bits at the *start* of a tick. Publishing
		// first means a value written between two ticks is still marked when it
		// is read, and the tick then clears exactly what was sent. Publishing
		// after the tick instead loses every write an author makes, because
		// nothing in an editor happens inside a system.
		void Step(PlayLink &link, int times = 1) {
			for (int index = 0; index < times; index++) {
				link.Step(Worlds);
				Worlds.Tick(FRAME_SECONDS);
			}
		}

		// Where one entity is on the replica, or nothing when it never arrived.
		std::optional<float> ReplicaX(const PlayLink &link, Entity entity) {
			std::optional<float> found;
			Worlds.Enter(link.ReplicaWorld(), [&found, entity](Store &store) {
				if (const auto *transform = store.Get<Transform>(entity)) {
					found = transform->Frame.Position.X;
				}
			});
			return found;
		}
	};
}

TEST_CASE("a play link gives the run a second world", "[studio][playlink]") {
	Fixture fixture;
	PlayLink link;

	CHECK_FALSE(link.IsRunning());

	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));
	CHECK(error.empty());
	CHECK(link.IsRunning());

	// A real world in the same universe, and a different one from the
	// authority. Two views of one store cannot show a divergence, which is the
	// only thing this panel is for.
	REQUIRE(link.ReplicaWorld().IsValid());
	CHECK(link.ReplicaWorld() != fixture.Authority);
	CHECK(fixture.Worlds.NameOf(link.ReplicaWorld()) != fixture.Worlds.NameOf(fixture.Authority));
}

TEST_CASE("the client view is marked as somebody else's world, both ways", "[studio][playlink]") {
	Fixture fixture;
	PlayLink link;

	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));

	// **Two records of one fact, and both are load-bearing.** `world::Replica`
	// refuses the bus writes at the call; `AdoptOnly` refuses minting an entity
	// in the storage. A replica that could mint one would allocate the index the
	// authority is about to allocate, and `Store::Apply` would then be right to
	// merge two different entities into one.
	bool refusesBus = false;
	bool refusesMinting = false;
	fixture.Worlds.Enter(link.ReplicaWorld(), [&](Store &store) {
		refusesBus = store.Resource<engine::world::Replica>() != nullptr;
		refusesMinting = store.AdoptOnly();
	});

	CHECK(refusesBus);
	CHECK(refusesMinting);
}

TEST_CASE("what the server holds arrives on the client", "[studio][playlink]") {
	Fixture fixture;

	const Entity near = fixture.Spawn(1.0f);
	const Entity far = fixture.Spawn(9.0f);

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));

	// Long enough for the join snapshot, which is deliberately spread over
	// several ticks rather than sent at once.
	fixture.Step(link, 32);

	// **The assertion the whole feature is for.** Not "a world exists" and not
	// "a message was produced" — the rows the authority holds are on the other
	// side, at the values it holds them at.
	const std::optional<float> nearX = fixture.ReplicaX(link, near);
	const std::optional<float> farX = fixture.ReplicaX(link, far);

	REQUIRE(nearX.has_value());
	REQUIRE(farX.has_value());
	CHECK_THAT(*nearX, Catch::Matchers::WithinAbs(1.0f, WIRE_MILLIMETRES));
	CHECK_THAT(*farX, Catch::Matchers::WithinAbs(9.0f, WIRE_MILLIMETRES));

	const studio::LinkReport &report = link.Report();
	CHECK(report.ServerEntities == 2);
	CHECK(report.ClientEntities == 2);

	// **One message, and that is the assertion rather than a disappointment.**
	// This world has no systems in it, so once the join has landed nothing
	// changes and there is nothing to send — a link that went on producing
	// deltas for a world at rest would be spending a client's bandwidth to tell
	// it what it already knows. `TotalMessages` is what says so.
	//
	// Deliberately *not* `Applied > 0`. This world was published before it had
	// ever ticked, so its snapshot is stamped tick 0 and a correct join leaves
	// `Applied` at zero — the assertion was written before that was understood
	// and it failed for a world that had replicated perfectly. See
	// `LinkReport::Applied`, which used to make the same claim.
	CHECK(report.TotalMessages == 1);
	CHECK(report.TotalBytes > 0);
}

TEST_CASE("a value written after the join crosses too", "[studio][playlink]") {
	Fixture fixture;
	const Entity entity = fixture.Spawn(0.0f);

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));
	fixture.Step(link, 32);

	const std::optional<float> joined = fixture.ReplicaX(link, entity);
	REQUIRE(joined.has_value());
	CHECK_THAT(*joined, Catch::Matchers::WithinAbs(0.0f, WIRE_MILLIMETRES));

	// **Through `Set`, because that is what marks it.** A system writing through
	// `Each`'s mutable reference marks nothing dirty — v0.3 wrote that up as one
	// of the four bugs a passing in-process suite could not see — so a test that
	// wrote the fast way would be asserting the delta path against a write the
	// delta path cannot see.
	fixture.Worlds.Enter(fixture.Authority, [entity](Store &store) {
		store.Set<Transform>(entity, Transform{CFrame(Vector3{42.0f, 0.0f, 0.0f})});
	});

	fixture.Step(link, 4);

	// The delta, rather than the snapshot. This is the half that fails when
	// nothing marks a write, and it fails by the client keeping a stale value
	// for ever rather than by anything reporting an error.
	const std::optional<float> moved = fixture.ReplicaX(link, entity);
	REQUIRE(moved.has_value());
	CHECK_THAT(*moved, Catch::Matchers::WithinAbs(42.0f, WIRE_MILLIMETRES));

	// A delta was built, sent and applied in full — which is what moves
	// `Applied` off the snapshot's tick, and the only unambiguous evidence that
	// this arrived as a change rather than in a fresh snapshot.
	CHECK(link.Report().Applied > 0);
	CHECK(link.Report().TotalMessages > 1);
}

TEST_CASE("stopping takes the client view away with it", "[studio][playlink]") {
	Fixture fixture;
	fixture.Spawn(3.0f);

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));
	fixture.Step(link, 8);

	const WorldId replica = link.ReplicaWorld();
	const size_t before = fixture.Worlds.Count();

	link.Stop(fixture.Worlds);

	// **The world goes, and the handle stops naming it.** Everything that asks
	// `Editor::IsReplicaWorld` — the worlds panel, the lifecycle, the save path
	// — would otherwise be answering about a hole.
	CHECK_FALSE(link.IsRunning());
	CHECK_FALSE(link.ReplicaWorld().IsValid());
	CHECK(fixture.Worlds.Count() == before - 1);
	CHECK(fixture.Worlds.NameOf(replica) == Name{});

	// Stopping twice is not an error. Stop is reached from a menu, a keybind and
	// the editor shutting down, and the third one runs after the first two.
	link.Stop(fixture.Worlds);
	CHECK_FALSE(link.IsRunning());
}

TEST_CASE("a link refuses to start twice and refuses a world that is not there", "[studio][playlink]") {
	Fixture fixture;
	PlayLink link;

	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));

	// A second start would leak the first replica world and leave the editor
	// holding a link to a world nothing would ever destroy.
	CHECK_FALSE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));
	CHECK_FALSE(error.empty());

	PlayLink other;
	std::string otherError;
	CHECK_FALSE(other.Start(fixture.Worlds, WorldId{}, TICK_RATE, otherError));
	CHECK_FALSE(otherError.empty());
	CHECK_FALSE(other.IsRunning());
}

// **A mirror crossing the wire, which is the whole of what a replica was
// missing.** `REPLICATED` carried `Transform`, `Motion`, `Bounds` and `Visual`,
// so a pane arrived with its `Surface` set and nothing on the client could
// render into that surface: the camera was not replicated and neither was the
// parent link that says which pane it projects off. Every mirror in a played
// world was a plain white part, and it looked like the renderer.
//
// The three components below are what closed it. The *aim* is deliberately not
// among them — see `client::AimReplicaViewer` — because a reflection is of the
// viewer and every client has its own.
TEST_CASE("a mirror arrives on the client whole", "[studio][playlink]") {
	Fixture fixture;

	engine::scene::RegisterSceneClasses();

	Entity pane;
	Entity reflection;
	fixture.Worlds.Enter(fixture.Authority, [&pane, &reflection](Store &store) {
		pane = store.CreateInstance(engine::scene::PartClass(), "Pane");
		store.Set<Transform>(pane, Transform{CFrame(Vector3::Zero)});
		store.Set<Bounds>(pane, Bounds{Vector3{8.0f, 4.5f, 0.2f}});
		store.Set<Visual>(pane, Visual{});

		reflection = store.CreateInstance(engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection");

		// **No slot set here, because nothing authors one.** A pane is a mirror
		// because a `SurfaceCamera` is parented to it, and
		// `scene::AimSurfaceCameras` hands the slots out — so what this fixture
		// has to get right is the face and the parent link, which are the two
		// things the wire has to carry.
		engine::scene::SurfaceCamera target;
		target.Face = engine::scene::NormalId::Front;
		store.Set(reflection, target);
		store.Set(reflection, engine::scene::Camera{});

		REQUIRE(store.SetParent(reflection, pane));
	});

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));
	fixture.Step(link, 32);

	fixture.Worlds.Enter(link.ReplicaWorld(), [pane, reflection](Store &store) {
		// The camera itself, with the face it projects off.
		const auto *target = store.Get<engine::scene::SurfaceCamera>(reflection);
		REQUIRE(target != nullptr);
		CHECK(target->Face == engine::scene::NormalId::Front);

		// Its lens, without which there is no projection to render with.
		CHECK(store.Get<engine::scene::Camera>(reflection) != nullptr);

		// **And the tree, which is the one the old list had no way to carry.**
		// `SurfaceCamera` names a face; *whose* face comes from the parent link
		// and nowhere else, so a camera that arrived without one could not be
		// aimed at all. `ecs.Hierarchy` had only the automatic name
		// `Components::Of` mints from the compiler's spelling until v0.8, which
		// is unusable on a wire because nothing makes two processes agree on it.
		CHECK(store.ParentOf(reflection) == pane);
	});

	// **Aimed from a camera the client made for itself.** A replica may not mint
	// an authoritative entity, so this comes out of the predicted range — and it
	// has to exist before `AimSurfaceCameras` will do anything, because a mirror
	// with no viewer has no reflection to compute rather than a default one.
	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		CHECK(engine::scene::AimSurfaceCameras(store) == 0);

		const Entity viewer =
			client::AimReplicaViewer(store, CFrame(Vector3{0.0f, 0.0f, 20.0f}), engine::scene::Camera{});
		REQUIRE(viewer != engine::ecs::NULL_ENTITY);

		CHECK(engine::scene::AimSurfaceCameras(store) == 1);
	});

	// The pane is told what it shows, on the client, by the client. The face is
	// at z = -0.2 and the eye at z = 20, so the reflection lands at z = -20.4 —
	// the same arithmetic `scene/tests/SurfaceCameras.cpp` pins, reached here
	// through the wire rather than through a parent set in this process.
	// **Slot 0, and derived rather than received.** The number is not on the
	// wire at all now — each end hands out slots in entity order, and a replica
	// matches entities by index and generation, so both arrive at the same
	// answer without sending it. One camera therefore means slot 0 on both
	// sides, and the two ends of the pairing are written from one variable.
	fixture.Worlds.Enter(link.ReplicaWorld(), [pane, reflection](Store &store) {
		CHECK(store.Get<Visual>(pane)->Surface == 0);
		CHECK(store.Get<engine::scene::SurfaceCamera>(reflection)->Surface == 0);

		const Vector3 placed = store.Get<Transform>(reflection)->Frame.Position;
		CHECK_THAT(placed.Z, Catch::Matchers::WithinAbs(-20.4f, 0.001f));
	});

	std::vector<engine::render::SurfaceView> views;
	fixture.Worlds.Enter(link.ReplicaWorld(), [&views](Store &store) {
		CHECK(client::CollectSurfaceViews(store, views) == 1);
	});

	REQUIRE(views.size() == 1);
	CHECK(views.front().Index == 0);
}

TEST_CASE("two clients get two worlds with two names", "[studio][playlink]") {
	// **What multi-client Play is made of.** Each client is an independent
	// link with its own replica world, because the bug a second client exists
	// to catch is the two of them disagreeing — and two views of one store
	// cannot disagree.
	//
	// The names have to differ for rule 4's reason: a world's name is its
	// identity to the universe, the worlds panel and any recording, so two
	// clients sharing one would be two worlds nothing could tell apart.
	Fixture fixture;
	PlayLink first;
	PlayLink second;

	std::string error;
	REQUIRE(first.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 1"));
	REQUIRE(second.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 2"));

	REQUIRE(first.ReplicaWorld().IsValid());
	REQUIRE(second.ReplicaWorld().IsValid());

	CHECK(first.ReplicaWorld() != second.ReplicaWorld());
	CHECK(fixture.Worlds.NameOf(first.ReplicaWorld()) != fixture.Worlds.NameOf(second.ReplicaWorld()));

	// And both are replicas of the same authority, which is what makes them
	// comparable rather than two unrelated scenes.
	CHECK(fixture.Worlds.NameOf(first.ReplicaWorld()) != fixture.Worlds.NameOf(fixture.Authority));
	CHECK(fixture.Worlds.NameOf(second.ReplicaWorld()) != fixture.Worlds.NameOf(fixture.Authority));

	first.Stop(fixture.Worlds);
	second.Stop(fixture.Worlds);
}

TEST_CASE("what the server holds arrives on every client", "[studio][playlink]") {
	// The property a single replica cannot demonstrate: the authority's state
	// reaches *each* client independently rather than one of them.
	Fixture fixture;
	PlayLink first;
	PlayLink second;

	std::string error;
	REQUIRE(first.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 1"));
	REQUIRE(second.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 2"));

	fixture.Spawn(2.0f);

	for (int beat = 0; beat < 8; beat++) {
		fixture.Worlds.Tick(1.0f / static_cast<float>(TICK_RATE));
		first.Step(fixture.Worlds);
		second.Step(fixture.Worlds);
	}

	CHECK(first.Report().ClientEntities == first.Report().ServerEntities);
	CHECK(second.Report().ClientEntities == second.Report().ServerEntities);
	CHECK(first.Report().ClientEntities > 0);
	CHECK(second.Report().ClientEntities > 0);

	first.Stop(fixture.Worlds);
	second.Stop(fixture.Worlds);
}

TEST_CASE("a play link admits a player and gives it a body", "[studio][playlink]") {
	// **What made Play "both halves" rather than one half wearing both hats.**
	// A link has held an authority and a replica since v0.7 and nobody was in
	// the replica — it received state and had no player, no character and no way
	// to move one. These are the three facts that changed, and each of them is
	// silently wrong in a different way if it is missing:
	//
	//   * no player, and the client is a spectator;
	//   * no `LocalPlayer` in the replica, and the client cannot tell which of
	//     the characters it is looking at is its own;
	//   * no route for its intent, and the keyboard reaches nothing.
	Fixture fixture;

	// A furnished world, because a player is a child of the `Players` service —
	// a scene nobody furnished gets no player, quietly, which is the placeholder
	// case `mono.server` names too.
	fixture.Worlds.Enter(fixture.Authority, [](Store &store) {
		engine::scene::RegisterSceneClasses();
		engine::scene::InstallServices(store);
	});

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));

	const Entity player = link.Player();
	REQUIRE(player != engine::ecs::NULL_ENTITY);

	Entity root;
	Entity humanoid;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		const Entity model = engine::scene::CharacterOf(store, player);
		REQUIRE(model != engine::ecs::NULL_ENTITY);

		const auto *rig = store.Get<engine::scene::Character>(model);
		REQUIRE(rig != nullptr);
		root = rig->Root;
		humanoid = rig->Humanoid;

		// Handed to the player, which is what makes a client's own movement
		// authoritative and nobody else's.
		CHECK(engine::scene::NetworkOwnerOf(store, root) == player);
	});

	// The replica is told which player is its own. It cannot be replicated — a
	// resource is one row and the answer differs per client — so this is the
	// studio's version of `game::JoinNotice`.
	fixture.Worlds.Enter(link.ReplicaWorld(), [player](Store &store) {
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		REQUIRE(local != nullptr);
		CHECK(local->Instance == player);
	});

	// The character crosses whole, which is a stronger claim than "rows
	// arrived": `Character` names two entities, and a replica holding the
	// component without the rows it points at is a character with no body.
	fixture.Step(link, 6);
	fixture.Worlds.Enter(link.ReplicaWorld(), [player](Store &store) {
		const Entity model = engine::scene::CharacterOf(store, player);
		REQUIRE(model != engine::ecs::NULL_ENTITY);
		REQUIRE(store.Get<engine::scene::Character>(model) != nullptr);
		CHECK(store.Alive(store.Get<engine::scene::Character>(model)->Root));
	});

	// --- and the keyboard reaches it -------------------------------------
	//
	// `Editor::DrivePlayer` writes the replica's `InputState` and needs a
	// window; what it writes is an ordinary resource, so a test writes the same
	// thing and the rest of the path is the real one — `ReadMoveIntent` in the
	// replica, the codec, `ApplyMoveInput` on the authority.
	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		auto *input = store.ResourceMutable<engine::scene::InputState>();
		REQUIRE(input != nullptr);
		input->Focused = true;
		input->Down.Set(engine::scene::KeyCode::W, true);
	});

	fixture.Step(link);

	fixture.Worlds.Enter(fixture.Authority, [humanoid](Store &store) {
		const auto *body = store.Get<engine::scene::Humanoid>(humanoid);
		REQUIRE(body != nullptr);

		// **Non-zero and normalised**, which is the two halves of the contract:
		// the intent crossed at all, and the host normalised it rather than
		// trusting whatever length arrived.
		CHECK_THAT(body->MoveDirection.Magnitude(), Catch::Matchers::WithinAbs(1.0, 0.001));
	});

	// Stopping takes the player with it, or a body is owned for ever by
	// somebody who has gone — which is the case `scene.ownership`'s reclaim
	// exists for and the one this must not create.
	link.Stop(fixture.Worlds);
	fixture.Worlds.Enter(fixture.Authority, [player, root](Store &store) {
		CHECK_FALSE(store.Alive(player));
		CHECK_FALSE(store.Alive(root));
	});
}

TEST_CASE("a played character walks where the keyboard points it", "[studio][playlink]") {
	// **The half above this one stops at `Humanoid::MoveDirection`.** That is a
	// field, not a displacement: everything from the key to the field can be
	// perfect and the character still stand still, because walking needs the
	// authority to have been furnished with a physics world, a weight, and the
	// character passes that turn a direction into a velocity. The case above
	// asserts the field; this one asserts the metres, and the two together are
	// the whole of what "press W and it walks" means.
	//
	// **The world is furnished the way `Editor::BuildWorld` furnishes one**, and
	// naming the same calls in the same order is the point — a studio that stops
	// installing one of them is a studio where Play looks exactly like this test
	// failing.
	Fixture fixture;

	fixture.Worlds.Enter(fixture.Authority, [](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::RegisterSceneClasses();

		// **`InstallPresentation` first, exactly as `Editor::BuildWorld` calls
		// it, and leaving it out is what made this case pass through the bug it
		// was written to catch.** It brings `client::InstallControls` with it,
		// and that installs `character-control` — `scene::UpdateCharacterControl`
		// running on the *authority*, against a keyboard the authority does not
		// have.
		//
		// A fixture that listed only the calls below had no second writer, so
		// the humanoid's `MoveDirection` was whatever `PlayLink` last applied
		// and the character walked. In the editor the local pass ran a few
		// systems later and wrote its empty direction over it every tick.
		// Naming the same calls in the same order is not a tidiness rule here;
		// it is the only reason this test can see the failure at all.
		client::InstallPresentation(store, systems, 256);

		engine::scene::InstallServices(store);

		engine::physics::PreparePhysicsWorld(store);
		engine::physics::RegisterPhysicsSystems(systems);

		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(systems);

		engine::scene::RegisterOwnershipSystem(systems);

		// Something to stand on. Without it the character falls for the whole
		// test and `Humanoid::Grounded` is never true, which is a different
		// failure wearing the same face.
		engine::scene::PartDesc floor;
		floor.Size = Vector3{200.0f, 4.0f, 200.0f};
		floor.Frame = CFrame(Vector3{0.0f, -2.0f, 0.0f});
		floor.Anchored = true;

		const Entity ground = engine::scene::MakePart(store, floor);
		store.SetInstanceName(ground, "SpawnLocation");
		store.SetParent(ground, engine::scene::WorkspaceOf(store));
	});

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error));

	const Entity player = link.Player();
	REQUIRE(player != engine::ecs::NULL_ENTITY);

	Entity root;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		const auto *rig = store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, player));
		REQUIRE(rig != nullptr);
		root = rig->Root;
	});

	// Let it settle onto the floor first, so what is measured below is walking
	// and not the tail of the drop.
	fixture.Step(link, 30);

	Vector3 before;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		const auto *placement = store.Get<Transform>(root);
		REQUIRE(placement != nullptr);
		before = placement->Frame.Position;

		const auto *body = store.Get<engine::scene::Humanoid>(
			store.Get<engine::scene::Character>(engine::scene::CharacterOf(store, player))->Humanoid
		);
		REQUIRE(body != nullptr);

		// **Standing on the floor, and this is not a formality.** `Grounded` is
		// the whole of what gates a jump, and `physics::GroundCharacters` gets it
		// from a ray that starts inside the character's own collider — so a
		// version of that function which rejects the caster by comparing the
		// result rather than by excluding it from the query answers "falling"
		// while the character rests perfectly still on a plate.
		CHECK(body->Grounded);
	});

	// The keyboard, written where `Editor::DrivePlayer` writes it.
	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		auto *input = store.ResourceMutable<engine::scene::InputState>();
		REQUIRE(input != nullptr);
		input->Focused = true;
		input->Down.Set(engine::scene::KeyCode::W, true);
	});

	fixture.Step(link, 30);

	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		const auto *placement = store.Get<Transform>(root);
		REQUIRE(placement != nullptr);

		const Vector3 walked = placement->Frame.Position - before;

		// **Half a second at `WalkSpeed`, so metres and not millimetres.** A
		// character that drifted a hair would pass a "not equal" check and would
		// still be a character that does not walk.
		CHECK(Vector3{walked.X, 0.0f, walked.Z}.Magnitude() > 2.0f);
	});

	// **And the client sees it, which is the half somebody actually looks at.**
	// The authority moving its own row proves the simulation; a person pressing
	// W is watching a *replica*, and the two are only the same picture if the
	// new pose is sent. It is not sent unless the authority is observing
	// `scene::Transform` — the delta for an `Observed` component is built from
	// the dirty bits and from nothing else — so a world nobody declared that on
	// replicates its join snapshot and then never moves again. Which is a
	// character that walks everywhere except on the screen.
	fixture.Worlds.Enter(link.ReplicaWorld(), [&](Store &store) {
		const auto *seen = store.Get<Transform>(root);
		REQUIRE(seen != nullptr);

		const Vector3 there = seen->Frame.Position;
		const Vector3 shown = Vector3{there.X - before.X, 0.0f, there.Z - before.Z};
		CHECK(shown.Magnitude() > 2.0f);
	});

	// --- and the space bar leaves the ground -----------------------------
	//
	// **The other key a player presses, and the one that fails on its own.**
	// Walking writes a horizontal velocity whatever the ground says; jumping
	// reads `Humanoid::Grounded` and does nothing without it. So a broken ground
	// query is invisible to every check above and is the whole of the bug to
	// somebody holding the space bar.
	//
	// **From a standstill, and the wait is the point.** A character that has
	// just walked is awake, and a jump off an awake body proves nothing about
	// the commonest case there is: somebody stands still, the solver rests the
	// body and `physics::Publish` takes its `scene::Motion` away, and *then*
	// they press space. Jumping is the only input that has to wake a body it did
	// not already move.
	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		auto *input = store.ResourceMutable<engine::scene::InputState>();
		REQUIRE(input != nullptr);
		input->Previous = {};
		input->Down = {};
	});

	fixture.Step(link, 180);

	bool slept = false;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		slept = !store.Has<engine::scene::Motion>(root);
	});
	INFO("the body never came to rest, so this case is not testing what it says");
	CHECK(slept);

	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		auto *input = store.ResourceMutable<engine::scene::InputState>();
		REQUIRE(input != nullptr);
		input->Down = {};

		// **An edge, so the previous frame has to not hold it.**
		// `scene::ReadMoveIntent` asks for the space bar's *tap*, and a tap is
		// an edge the writer recorded — a key held since the last frame is not
		// a jump, and a bit set in `Down` that no `LatchPresses` ever saw is not
		// a press at all. `Editor::DrivePlayer` ends its frame with that call,
		// so the harness does too.
		input->Previous = {};
		input->Down.Set(engine::scene::KeyCode::Space, true);
		input->LatchPresses();
	});

	// **The whole arc, not the first half.** A jump is up *and* down, and the
	// two halves fail apart: the rise is `Humanoid::JumpSpeed` and `Grounded`,
	// and the return is gravity still reaching a body that is touching nothing.
	// Sampled every tick because the apex is between two of them.
	//
	// Two seconds is the budget, and it is a statement about the numbers rather
	// than a guess. `JumpSpeed` is chosen against `CHARACTER_HEIGHT` and
	// `scene::Gravity` so that the whole arc takes well under a second and a
	// half — a jump that has not landed by now is one whose speed and gravity
	// disagree about what units they are in, which is exactly the state that
	// reads as a character frozen in the air.
	// **Released after one frame, and forgetting to was worth catching.** The
	// replica's `InputState` is written by `Editor::DrivePlayer` every frame in
	// a real editor; a test that sets it once and walks away leaves `Down`
	// holding space and `Previous` not, so `WasKeyPressed` answers yes for ever
	// and the character jumps again the tick it lands. Which looks a great deal
	// like a bug in the jump and is a bug in the harness.
	fixture.Step(link);
	fixture.Worlds.Enter(link.ReplicaWorld(), [](Store &store) {
		auto *input = store.ResourceMutable<engine::scene::InputState>();
		input->Previous = input->Down;
		input->Down = {};
	});

	float apex = before.Y;
	float landed = before.Y;
	for (int beat = 0; beat < 120; beat++) {
		fixture.Step(link);
		fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
			if (const auto *placement = store.Get<Transform>(root)) {
				apex = std::max(apex, placement->Frame.Position.Y);
				landed = placement->Frame.Position.Y;
			}
		});
	}

	// Clear of where it stood, rather than merely different: a character bobbing
	// on a contact would pass anything tighter. And under a storey, because a
	// jump measured in tens of metres is the units mistake this bounds.
	CHECK(apex > before.Y + 1.0f);
	CHECK(apex < before.Y + 10.0f);

	// **And it is back on the floor**, which is the half that catches the hang.
	CHECK_THAT(landed, Catch::Matchers::WithinAbs(before.Y, 0.5));

	// **The client watched the whole arc, and this is the case somebody
	// actually reported.** A jumping character touches nothing, and
	// `physics::Publish` used to mark `scene::Transform` changed only for the
	// bodies the solver had a manifold for — so the moment it left the ground it
	// stopped replicating. The client saw it rise for the two or three ticks the
	// floor contact survived take-off, and then hang at that height until it
	// landed and a contact put it back on the wire. Which reads exactly as "it
	// jumps, then freezes in mid-air and never comes down".
	fixture.Worlds.Enter(link.ReplicaWorld(), [&](Store &store) {
		const auto *seen = store.Get<Transform>(root);
		REQUIRE(seen != nullptr);
		CHECK_THAT(seen->Frame.Position.Y, Catch::Matchers::WithinAbs(landed, 0.5));
	});

	link.Stop(fixture.Worlds);
}

// --- the studio's own half --------------------------------------------------
//
// **The header above says a viewport needs a window and a device, and that is
// half true.** What a viewport *draws* does. What it does with the keyboard does
// not: `Editor::DrivePlayer` is imgui state and store writes, and imgui runs
// perfectly well with a context, a font atlas and no backend at all. So the one
// step of "press W and the character walks" that had no test — the editor
// deciding that this panel's keyboard belongs to that client — is testable, and
// the gap is why "I click the client viewport and nothing happens" could not be
// reproduced anywhere but by hand.

namespace {
	// An imgui context with no backend, and a frame open on it.
	//
	// The font atlas is built explicitly because `NewFrame` asserts on one that
	// is not — a backend would have done it while uploading the texture, and
	// there is no backend here.
	struct Frame {
		ImGuiContext *Context = nullptr;

		Frame() {
			Context = ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			io.DisplaySize = ImVec2{1280.0f, 720.0f};
			io.DeltaTime = FRAME_SECONDS;

			unsigned char *pixels = nullptr;
			int width = 0;
			int height = 0;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			io.Fonts->SetTexID(static_cast<ImTextureID>(1));
		}

		~Frame() {
			if (Context != nullptr) {
				ImGui::DestroyContext(Context);
			}
		}

		Frame(const Frame &) = delete;
		Frame &operator=(const Frame &) = delete;

		// Opens a frame with `key` held, and closes the previous one.
		void HoldKey(ImGuiKey key, bool down) {
			ImGui::NewFrame();
			ImGui::GetIO().AddKeyEvent(key, down);
			ImGui::EndFrame();

			// A second frame, so the event queued above is in `KeysData` while
			// the frame is open — which is the state `DrivePlayer` reads.
			ImGui::NewFrame();
		}
	};
}

TEST_CASE("a client viewport hands its keyboard to the character", "[studio][playlink]") {
	Frame frame;

	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();

	engine::parallel::Jobs::Start(1);
	engine::scene::RegisterSceneComponents();

	WorldSettings settings;
	settings.Name = Name("Scene");
	settings.TickRate = TICK_RATE;
	const WorldId authority = editor.Universe->Create(settings);

	// Furnished as `Editor::BuildWorld` furnishes one, which is what makes the
	// answer below an answer about the editor rather than about this fixture.
	editor.Universe->Enter(authority, [](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::RegisterSceneClasses();
		engine::scene::InstallServices(store);
		engine::physics::PreparePhysicsWorld(store);
		engine::physics::RegisterPhysicsSystems(systems);
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(systems);
		engine::scene::RegisterOwnershipSystem(systems);
		engine::physics::RegisterCharacterSystems(systems);

		engine::scene::PartDesc floor;
		floor.Size = Vector3{200.0f, 4.0f, 200.0f};
		floor.Frame = CFrame(Vector3{0.0f, -2.0f, 0.0f});
		floor.Anchored = true;

		const Entity ground = engine::scene::MakePart(store, floor);
		store.SetInstanceName(ground, "SpawnLocation");
		store.SetParent(ground, engine::scene::WorkspaceOf(store));
	});

	auto link = std::make_unique<PlayLink>();
	std::string error;
	REQUIRE(link->Start(*editor.Universe, authority, TICK_RATE, error, "client 1"));

	const WorldId replica = link->ReplicaWorld();

	// **Recorded as a run, because that is how the editor knows a world is a
	// client's.** `Editor::IsReplicaWorld` walks `Runs` and nothing else, so a
	// link the editor is not holding is a panel `DrivePlayer` refuses on its
	// first line — which is indistinguishable, from the outside, from a
	// keyboard that is simply ignored.
	studio::Editor::WorldRun run;
	run.World = authority;
	run.Mode = studio::RunMode::Play;
	run.Links.push_back(std::move(link));
	editor.Runs.push_back(std::move(run));

	PlayLink &live = *editor.Runs.front().Links.front();

	// The character has to have crossed before the keyboard means anything:
	// `DrivePlayer` refuses a viewport whose client has no body, deliberately,
	// so that the panel still flies a free camera in the gap.
	for (int beat = 0; beat < 8; beat++) {
		live.Step(*editor.Universe);
		editor.Universe->Tick(FRAME_SECONDS);
	}

	editor.Universe->Enter(replica, [](Store &store) {
		const auto *local = store.Resource<engine::scene::LocalPlayer>();
		REQUIRE(local != nullptr);
		INFO("the client never learned which player is its own");
		REQUIRE(engine::scene::CharacterOf(store, local->Instance) != engine::ecs::NULL_ENTITY);
	});

	frame.HoldKey(ImGuiKey_W, true);

	// Hovered, which is what a pointer over the panel gives it. Not focused and
	// not active: the pointer alone has to be enough, because that is the rule
	// `DriveCamera` resolves the target with.
	CHECK(editor.DrivePlayer(replica, true, false, false));

	editor.Universe->Enter(replica, [](Store &store) {
		const auto *input = store.Resource<engine::scene::InputState>();
		REQUIRE(input != nullptr);

		// **The three facts a character needs and the editor is the only source
		// of.** Focus, because `ReadMoveIntent` refuses an unfocused world; the
		// key, because that is the whole message; and an intent that is not
		// zero, because the first two can both be right and still produce a
		// direction of nothing if the key table names the wrong code.
		CHECK(input->Focused);
		CHECK(input->IsKeyDown(engine::scene::KeyCode::W));
		CHECK(engine::scene::ReadMoveIntent(store).Direction.Magnitude() > 0.5f);
	});

	// **And the panel that is not being pointed at drives nobody.** Two client
	// views in one editor must not both walk on one keyboard — the second would
	// be a character somebody is not looking at, moving on a key meant for the
	// first.
	CHECK_FALSE(editor.DrivePlayer(replica, false, false, false));
	editor.Universe->Enter(replica, [](Store &store) {
		CHECK_FALSE(store.Resource<engine::scene::InputState>()->Focused);
	});

	// A world that is not a client's is never played, whatever is held over it:
	// the authority's own panel is the server's view and WASD flies its camera.
	CHECK_FALSE(editor.DrivePlayer(authority, true, false, true));

	editor.Runs.clear();
	editor.Universe.reset();
	engine::parallel::Jobs::Stop();
}

TEST_CASE("one client viewport walks and the others let go", "[studio][playlink]") {
	// **The bug this exists for did not stop the keyboard reaching the client —
	// it stopped it staying there.** `Editor::DriveCamera` picks the panel a
	// gesture means from `Hovered || Active || Panning` and used to hand only
	// the first two on, so a panel chosen *because* it was panning arrived at
	// `DrivePlayer` looking untouched: it took the frame, decided it was not
	// being driven, and wiped the keys. `Panning` survives a middle-drag
	// released off the picture, so the state is sticky — the character got a
	// move direction on a fraction of the ticks and, because
	// `scene::StepCharacters` replaces horizontal velocity rather than adding
	// to it, went nowhere at all.
	//
	// The other half is the panels nobody visited. One `DrivePlayer` call a
	// frame left every other client world holding the last keys it was given,
	// which is a second character walking for ever on a released key.
	Frame frame;

	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();

	engine::parallel::Jobs::Start(1);
	engine::scene::RegisterSceneComponents();

	WorldSettings settings;
	settings.Name = Name("Scene");
	settings.TickRate = TICK_RATE;
	const WorldId authority = editor.Universe->Create(settings);

	editor.Universe->Enter(authority, [](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::RegisterSceneClasses();
		engine::scene::InstallServices(store);
		engine::physics::PreparePhysicsWorld(store);
		engine::physics::RegisterPhysicsSystems(systems);
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(systems);
		engine::scene::RegisterOwnershipSystem(systems);
		engine::physics::RegisterCharacterSystems(systems);

		engine::scene::PartDesc floor;
		floor.Size = Vector3{200.0f, 4.0f, 200.0f};
		floor.Frame = CFrame(Vector3{0.0f, -2.0f, 0.0f});
		floor.Anchored = true;

		const Entity ground = engine::scene::MakePart(store, floor);
		store.SetInstanceName(ground, "SpawnLocation");
		store.SetParent(ground, engine::scene::WorkspaceOf(store));
	});

	// Two clients, because one cannot show either half of this.
	studio::Editor::WorldRun run;
	run.World = authority;
	run.Mode = studio::RunMode::Play;
	for (int client = 0; client < 2; client++) {
		auto link = std::make_unique<PlayLink>();
		std::string error;
		REQUIRE(link->Start(
			*editor.Universe, authority, TICK_RATE, error, "client " + std::to_string(client + 1)
		));
		run.Links.push_back(std::move(link));
	}
	editor.Runs.push_back(std::move(run));

	const WorldId first = editor.Runs.front().Links[0]->ReplicaWorld();
	const WorldId second = editor.Runs.front().Links[1]->ReplicaWorld();

	for (int beat = 0; beat < 8; beat++) {
		for (const std::unique_ptr<PlayLink> &link : editor.Runs.front().Links) {
			link->Step(*editor.Universe);
		}
		editor.Universe->Tick(FRAME_SECONDS);
	}

	// Two panels, one per client, laid out the way `Editor::SpawnPlayer` lays
	// them out.
	REQUIRE(editor.Extras.size() >= 2);
	editor.Extras[0].Open = true;
	editor.Extras[0].World = first;
	editor.Extras[1].Open = true;
	editor.Extras[1].World = second;

	// **The pointer is in the first panel and the second is stuck panning**,
	// which is the state a middle-drag released outside the picture leaves. The
	// search below must still choose the panel under the pointer, and the stuck
	// one must be told it has nothing rather than being handed the frame.
	editor.Extras[0].Hovered = true;
	editor.Extras[1].Panning = true;

	const auto intentIn = [&editor](WorldId world) {
		float magnitude = 0.0f;
		bool focused = false;
		editor.Universe->Enter(world, [&](Store &store) {
			magnitude = engine::scene::ReadMoveIntent(store).Direction.Magnitude();
			focused = store.Resource<engine::scene::InputState>()->Focused;
		});
		return std::pair<float, bool>{magnitude, focused};
	};

	frame.HoldKey(ImGuiKey_W, true);
	editor.DriveCamera();

	const auto [walking, hasKeyboard] = intentIn(first);
	CHECK(hasKeyboard);
	CHECK(walking > 0.5f);

	const auto [idle, stuck] = intentIn(second);
	CHECK_FALSE(stuck);
	CHECK(idle < 0.01f);

	// **And the pointer moving to the stuck panel hands it over rather than
	// finding it already holding the frame.** `Panning` counts as the pointer
	// being there, which is what the target search has always meant by it.
	editor.Extras[0].Hovered = false;
	editor.DriveCamera();

	CHECK(intentIn(second).first > 0.5f);
	CHECK(intentIn(first).first < 0.01f);

	// Nothing under the pointer and no viewport focused: everybody lets go.
	// Alt-tabbing away while holding W must not leave a character walking.
	editor.Extras[1].Panning = false;
	editor.DriveCamera();

	CHECK(intentIn(first).first < 0.01f);
	CHECK(intentIn(second).first < 0.01f);

	editor.Runs.clear();
	editor.Universe.reset();
	engine::parallel::Jobs::Stop();
}

TEST_CASE("a teleport wakes the world it arrives in, even outside the run", "[studio][playlink]") {
	// **A client walking onto a teleport pad used to end that client**, and the
	// three facts that make it happen are all ordinary on their own.
	//
	// `TeleportService:Teleport` destroys the player in the world they left
	// before the destination has admitted them — it has to, because only the
	// source world can, and a player left behind would be in two places at
	// once. The arrival is a payload sitting in the destination's inbox. And a
	// world that is not part of the run is suspended, so it never ticks, so it
	// never drains that inbox.
	//
	// The player therefore exists nowhere: destroyed in the source, unbuilt in
	// the destination. `Editor::FollowTeleports` searches every world for
	// `LOST_FRAMES`, finds nobody, and reports the client gone — which is what
	// the studio's own Playground does, because its pad names Arena and Arena
	// is not the scene being played.
	//
	// `UpdateWorldLifecycle` already knew how to answer this: a suspended world
	// with something in its letterbox is resumed, and it even says so in the
	// output log. It simply refused to look at a world outside the run before
	// deciding, which is the ordering this case pins.
	studio::Editor editor;
	editor.Universe = std::make_unique<Universe>();

	engine::parallel::Jobs::Start(1);
	engine::scene::RegisterSceneComponents();

	WorldSettings played;
	played.Name = Name("Scene");
	played.TickRate = TICK_RATE;
	const WorldId authority = editor.Universe->Create(played);

	// The destination, which nobody is playing. Exactly Arena's position in the
	// studio's new-game template.
	WorldSettings elsewhere;
	elsewhere.Name = Name("Arena");
	elsewhere.TickRate = TICK_RATE;
	const WorldId destination = editor.Universe->Create(elsewhere);

	editor.Universe->SetState(destination, engine::world::WorldState::Suspended);
	REQUIRE(editor.Universe->StateOf(destination) == engine::world::WorldState::Suspended);

	studio::Editor::WorldRun run;
	run.World = authority;
	run.Mode = studio::RunMode::Play;
	editor.Runs.push_back(std::move(run));

	// **Nothing has arrived yet, so the closed world stays closed.** Without
	// this half the case would pass against an editor that simply woke
	// everything, which is the fix nobody wants: it restarts scenes the author
	// deliberately stopped.
	editor.UpdateWorldLifecycle();
	CHECK(editor.Universe->StateOf(destination) == engine::world::WorldState::Suspended);

	// A teleport, as the router leaves one: a payload in the destination's
	// inbox, addressed from the world the player left.
	editor.Universe->Enter(destination, [](Store &store) {
		engine::world::Delivery arrival;
		arrival.Bus = engine::world::BusKind::Teleport;
		arrival.Key = Name("client 1");
		arrival.From = Name("Scene");

		// The resource itself is the driver's to create, and a world that has
		// never been delivered anything has none — so the arrival brings it,
		// exactly as the first delivery would.
		engine::world::Inbox inbox;
		inbox.Arrived.push_back(std::move(arrival));
		store.SetResource(std::move(inbox));
	});

	editor.UpdateWorldLifecycle();

	INFO("the arrival is still sitting in a world that will never tick to read it");
	CHECK(editor.Universe->StateOf(destination) == engine::world::WorldState::Active);

	editor.Runs.clear();
	editor.Universe.reset();
	engine::parallel::Jobs::Stop();
}

TEST_CASE("a client that leaves takes its character with it", "[studio][playlink]") {
	// **A body outstays its owner and nothing ever collects it.** The character
	// is a `Model` under Workspace, so it is not reachable from the `Player`
	// once that instance is gone — `RemoveCharacter` is the only thing that
	// knows the two are connected, and it has to run *before* the player is
	// destroyed or the link between them is already cut.
	//
	// Left behind, it is a rig with a `Humanoid` nobody drives, a root part the
	// solver keeps awake, and six limbs `PoseCharacters` follows for the rest of
	// the session. Two clients joining and leaving leave two of them standing on
	// the spawn.
	// Furnished as `Editor::BuildWorld` furnishes one, so the rig under test is
	// the seven-part one a real client gets rather than whatever a bare store
	// happens to build.
	Fixture fixture;

	fixture.Worlds.Enter(fixture.Authority, [](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::RegisterSceneClasses();
		client::InstallPresentation(store, systems, 256);
		engine::scene::InstallServices(store);

		engine::physics::PreparePhysicsWorld(store);
		engine::physics::RegisterPhysicsSystems(systems);
		engine::scene::PrepareGravity(store);
		engine::scene::RegisterGravitySystem(systems);
		engine::scene::RegisterOwnershipSystem(systems);

		engine::scene::PartDesc floor;
		floor.Size = Vector3{200.0f, 4.0f, 200.0f};
		floor.Frame = CFrame(Vector3{0.0f, -2.0f, 0.0f});
		floor.Anchored = true;

		const Entity ground = engine::scene::MakePart(store, floor);
		store.SetInstanceName(ground, "SpawnLocation");
		store.SetParent(ground, engine::scene::WorkspaceOf(store));
	});

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 1"));

	const Entity player = link.Player();
	REQUIRE(player != engine::ecs::NULL_ENTITY);

	fixture.Step(link, 8);

	Entity model = engine::ecs::NULL_ENTITY;
	size_t limbs = 0;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		model = engine::scene::CharacterOf(store, player);
		store.Each<const engine::scene::Humanoid>([&limbs](Entity, const engine::scene::Humanoid &) {
			limbs++;
		});
	});
	REQUIRE(model != engine::ecs::NULL_ENTITY);
	REQUIRE(limbs == 1);

	link.Stop(fixture.Worlds);

	// A tick after, because a rig torn down by a destroy has to survive the
	// systems that walk it — `PoseCharacters` follows a root it may no longer
	// have, and a limb outliving its model is exactly what this is looking for.
	fixture.Worlds.Tick(FRAME_SECONDS);

	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		CHECK_FALSE(store.Alive(player));

		INFO("the character is still standing there with nobody in it");
		CHECK_FALSE(store.Alive(model));

		// **The limbs and not only the model**, because destroying the root
		// alone leaves six parts following an entity that is not alive.
		size_t left = 0;
		store.Each<const engine::scene::Humanoid>([&left](Entity, const engine::scene::Humanoid &) {
			left++;
		});
		INFO("a humanoid outlived the player it belonged to");
		CHECK(left == 0);
	});
}

TEST_CASE("a player destroyed by anything else loses its character too", "[studio][playlink]") {
	// **The registration, not the function.** `scene::ReclaimOrphanedCharacters`
	// has its own cases in `mono.engine/scene/tests/Characters.cpp`; what this
	// one asks is whether anything ever calls it — which is the failure mode
	// that produced it, since the rule existed twice as a line two callers had
	// to remember and nowhere as a rule.
	//
	// So the player goes the way a script's `player:Destroy()` or an author
	// deleting one in the explorer would take it: no `RemoveCharacter`, no
	// `PlayLink::Stop`, just the instance gone and a tick afterwards.
	Fixture fixture;

	fixture.Worlds.Enter(fixture.Authority, [](Store &store, engine::ecs::Scheduler &systems) {
		engine::scene::RegisterSceneClasses();
		client::InstallPresentation(store, systems, 256);
		engine::scene::InstallServices(store);
	});

	PlayLink link;
	std::string error;
	REQUIRE(link.Start(fixture.Worlds, fixture.Authority, TICK_RATE, error, "client 1"));

	const Entity player = link.Player();
	REQUIRE(player != engine::ecs::NULL_ENTITY);

	fixture.Step(link, 4);

	Entity model = engine::ecs::NULL_ENTITY;
	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		model = engine::scene::CharacterOf(store, player);
	});
	REQUIRE(model != engine::ecs::NULL_ENTITY);

	fixture.Worlds.Enter(fixture.Authority, [player](Store &store) { store.DestroyInstance(player); });

	fixture.Worlds.Tick(FRAME_SECONDS);

	fixture.Worlds.Enter(fixture.Authority, [&](Store &store) {
		INFO("nothing in the tick collects a character whose player is gone");
		CHECK_FALSE(store.Alive(model));
	});

	link.Stop(fixture.Worlds);
}
