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

#include <engine/parallel/Jobs.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/world/Postbox.hpp>
#include <engine/world/Universe.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <client/Replicated.hpp>
#include <client/Scene.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Enums.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <studio/PlayLink.hpp>

#include <optional>

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

		reflection = store.CreateInstance(
			engine::ecs::Classes::Find(Name("SurfaceCamera")), "Reflection"
		);

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
