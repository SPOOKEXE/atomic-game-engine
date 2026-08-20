// The example scenes, loaded.
//
// **A scene that fails to load is one nobody notices is broken** until they run
// the client and see a black screen. Neither the shadow pass nor the surface
// pass can be asserted against without a GPU - `AGENTS.md` names that exception
// and refuses a mock renderer to close it on paper - so what this suite asserts
// is what can be: that each scene builds the *inputs* those passes need, in the
// world, through the same bindings a game would use.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/Layout.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/script/Instances.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.examples.scene")
TEST_DEPENDS("engine.script.scripting")

using Catch::Approx;
using engine::core::Name;
using engine::ecs::Entity;
using engine::ecs::Scheduler;
using engine::ecs::Store;
using engine::examples::ExamplePath;
using engine::examples::LoadScene;
using engine::scene::ActiveCamera;
using engine::scene::SurfaceCamera;
using engine::scene::Visual;
using engine::scene::WorldBounds;

namespace {

	// Where a script's content lives now.
	//
	// **`part.Parent = workspace` used to make a root and now makes a child of
	// the `Workspace` service**, so a lookup by root finds nothing. See
	// `script/LuauBindings.hpp`'s `OpenWorkspace` for why the two notions of "the
	// workspace" were collapsed, and `scene/Visibility.hpp` for what the tree
	// now decides.
	//
	// Falls back to a root, because some of these scripts deliberately leave an
	// instance unparented - an orphan is still reachable from C++ through
	// `EachRoot`, and only a *script* is unable to list one. A test about
	// signals or tasks should not have to care which of the two its fixture is.
	Entity InScene(Store &store, std::string_view name) {
		const Entity workspace = engine::scene::WorkspaceOf(store);
		if (workspace != engine::ecs::NULL_ENTITY) {
			if (const Entity child = store.FindFirstChild(workspace, name);
				child != engine::ecs::NULL_ENTITY) {
				return child;
			}
		}
		return store.FindFirstRoot(name);
	}
	// The staged assets root, not the test binary's own directory.
	//
	// `Paths::Assets` defaults to where the running executable sits, and a test
	// binary sits in `tests/` while the examples stage into `assets/`. A client
	// finds them because a client stages beside them; this has to be told.
	struct StagedAssets {
		std::filesystem::path Previous = engine::core::Paths::Assets();

		StagedAssets() {
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");
		}

		~StagedAssets() {
			engine::core::Paths::SetAssetsOverride(Previous);
		}
	};

	size_t CountNamed(Store &store, const char *name) {
		size_t found = 0;
		store.Each<const Visual>([&](Entity entity, const Visual &) {
			if (store.InstanceNameOf(entity) == Name(name)) {
				found++;
			}
		});
		return found;
	}

	// The same count over the 2D tree.
	//
	// **A separate helper rather than a wider query, because the two trees have
	// no component in common and that is the design.** A `Part` is named by its
	// `scene::Visual` and a `TextButton` by its `gui::Element`; a counter that
	// took both would have to name a class rather than a component, which is
	// the coupling `gui/AGENTS.md` spends a section refusing.
	size_t CountElements(Store &store, const std::string &name) {
		size_t found = 0;
		store.Each<const engine::gui::Element>([&](Entity entity, const engine::gui::Element &) {
			if (store.InstanceNameOf(entity) == Name(name)) {
				found++;
			}
		});
		return found;
	}

	// The first element of a given name, or null.
	Entity FirstElement(Store &store, const char *name) {
		Entity found = engine::ecs::NULL_ENTITY;
		store.Each<const engine::gui::Element>([&](Entity entity, const engine::gui::Element &) {
			if (found == engine::ecs::NULL_ENTITY && store.InstanceNameOf(entity) == Name(name)) {
				found = entity;
			}
		});
		return found;
	}
}

TEST_CASE("the rings scene builds and moves itself", "[examples][scene]") {
	const StagedAssets assets;

	Store store("rings");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Rings.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountNamed(store, "Orbiter") == 512);

	// The bounds are measured from what the script built rather than declared
	// by it, so a scene that produced nothing would frame a world one unit
	// across - which is exactly the bug the settle beat in `LoadScene` exists
	// for.
	REQUIRE(store.Resource<WorldBounds>() != nullptr);
	CHECK(store.Resource<WorldBounds>()->HalfExtent > 5.0f);
}

TEST_CASE("the mirrors scene builds what the render passes need", "[examples][scene]") {
	const StagedAssets assets;

	Store store("mirrors");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Mirrors-1-world.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The four walls, and the face each one points at the origin. **Stated here
	// as a table rather than checked one wall at a time**, because the thing
	// worth pinning is that all six `NormalId` axes resolve the same way: the
	// walls are unrotated, so a face is the inward normal directly, and a
	// regression that swapped `Left` and `Right` would leave a scene that still
	// loads and reflects the outside of the room.
	struct Wall {
		const char *Name;
		engine::scene::NormalId Face;
	};
	const Wall walls[] = {
		{"MirrorNorth", engine::scene::NormalId::Back},
		{"MirrorEast", engine::scene::NormalId::Left},
		{"MirrorWest", engine::scene::NormalId::Right},
		{"MirrorSouth", engine::scene::NormalId::Front},
	};

	Entity north = engine::ecs::NULL_ENTITY;
	Entity lowest = engine::ecs::NULL_ENTITY;

	for (const Wall &wall : walls) {
		INFO(wall.Name);

		const Entity pane = InScene(store, wall.Name);
		REQUIRE(pane != engine::ecs::NULL_ENTITY);

		// The surface camera **parented to the pane** rather than standing
		// beside it in the workspace. That is the arrangement `SurfaceCamera` is
		// for: the camera is placed off a face of its parent, so it is a child
		// of the thing it reflects.
		const Entity reflection = store.FindFirstChild(pane, "Reflection");
		REQUIRE(reflection != engine::ecs::NULL_ENTITY);

		const auto *surface = store.Get<SurfaceCamera>(reflection);
		REQUIRE(surface != nullptr);
		CHECK(surface->Face == wall.Face);

		// **Wide rather than square, and all four the same** - because all four
		// walls are the same shape, not because they share a target. Each index
		// owns its own pair since v0.8.
		CHECK(surface->Width == 2048);
		CHECK(surface->Height == 512);

		// **The pane is told what it shows by the aiming pass, not by the
		// script.** Before it runs there is nothing to sample, which is why this
		// asserts either side of the call rather than only after: a scene that
		// arrived with `Surface` already set would pass the later check while
		// proving nothing about the mechanism.
		CHECK(store.Get<Visual>(pane)->Surface == -1);

		if (lowest == engine::ecs::NULL_ENTITY || reflection.Id < lowest.Id) {
			lowest = reflection;
		}
		if (std::string_view(wall.Name) == "MirrorNorth") {
			north = reflection;
		}
	}

	CHECK(lowest == north);

	REQUIRE(engine::scene::AimSurfaceCameras(store) == 4);

	bool seen[engine::scene::MAX_SURFACES] = {};
	for (const Wall &wall : walls) {
		INFO(wall.Name);

		const Entity pane = InScene(store, wall.Name);
		const Entity reflection = store.FindFirstChild(pane, "Reflection");
		REQUIRE(reflection != engine::ecs::NULL_ENTITY);

		const int16_t index = store.Get<SurfaceCamera>(reflection)->Surface;
		REQUIRE(index >= 0);
		REQUIRE(static_cast<size_t>(index) < engine::scene::MAX_SURFACES);

		CHECK(store.Get<Visual>(pane)->Surface == index);

		CHECK_FALSE(seen[index]);
		seen[index] = true;
	}

	const auto *active = store.Resource<ActiveCamera>();
	REQUIRE(active != nullptr);
	CHECK(active->Entity == InScene(store, "Viewer"));
	CHECK(active->Entity != north);

	CHECK(InScene(store, "Baseplate") != engine::ecs::NULL_ENTITY);
	CHECK(CountNamed(store, "Caster") == 24);

	constexpr float FURTHEST = 13.0f;
	size_t counted = 0;
	store.Each<const engine::scene::Transform, const Visual>(
		[&](Entity entity, const engine::scene::Transform &placement, const Visual &) {
			if (store.InstanceNameOf(entity) != Name("Caster")) {
				return;
			}
			counted++;
			CHECK(std::abs(placement.Frame.Position.X) <= FURTHEST);
			CHECK(std::abs(placement.Frame.Position.Z) <= FURTHEST);
		}
	);
	CHECK(counted == 24);
}

TEST_CASE("the reflected camera is the eye mirrored through the plane", "[examples][scene]") {
	const StagedAssets assets;

	Store store("mirrors");
	Scheduler systems;

	std::string error;
	REQUIRE(LoadScene(store, systems, ExamplePath("Mirrors-1-world.luau"), error));

	REQUIRE(engine::scene::AimSurfaceCameras(store) == 4);

	const auto *viewer = store.Get<engine::scene::Transform>(InScene(store, "Viewer"));
	REQUIRE(viewer != nullptr);

	SECTION("the north wall, mirroring Z") {
		const Entity pane_ = InScene(store, "MirrorNorth");
		const auto *pane = store.Get<engine::scene::Transform>(pane_);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane_);
		const auto *reflected =
			store.Get<engine::scene::Transform>(store.FindFirstChild(pane_, "Reflection"));
		REQUIRE(pane != nullptr);
		REQUIRE(bounds != nullptr);
		REQUIRE(reflected != nullptr);

		const float plane = pane->Frame.Position.Z + bounds->HalfExtent.Z;

		CHECK(reflected->Frame.Position.Z == Approx(2.0f * plane - viewer->Frame.Position.Z));

		// And the two axes the plane does not mirror are unchanged.
		CHECK(reflected->Frame.Position.X == Approx(viewer->Frame.Position.X));
		CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
	}

	SECTION("the east wall, mirroring X") {
		const Entity pane_ = InScene(store, "MirrorEast");
		const auto *pane = store.Get<engine::scene::Transform>(pane_);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane_);
		const auto *reflected =
			store.Get<engine::scene::Transform>(store.FindFirstChild(pane_, "Reflection"));
		REQUIRE(pane != nullptr);
		REQUIRE(bounds != nullptr);
		REQUIRE(reflected != nullptr);

		const float plane = pane->Frame.Position.X - bounds->HalfExtent.X;

		CHECK(reflected->Frame.Position.X == Approx(2.0f * plane - viewer->Frame.Position.X));
		CHECK(reflected->Frame.Position.Y == Approx(viewer->Frame.Position.Y));
		CHECK(reflected->Frame.Position.Z == Approx(viewer->Frame.Position.Z));
	}
}

TEST_CASE("every portal shows the room it names", "[examples][scene]") {
	// **The one thing about a portal a headless test can decide, and it is the
	// thing that was wrong.** Where the camera stands is `scene`'s to assert and
	// `scene/tests/SurfaceCameras.cpp` does; what a *scene* gets wrong is which
	// part it points a hole at, and the failure is silent: `Face` is resolved on
	// the destination as well, so naming a wall whose matching face points out of
	// its room places a camera, fits a frustum, renders - and shows the empty
	// space behind that wall. Every assertion this suite had before would have
	// passed on it, and it did.
	//
	// The invariant that catches it needs no arithmetic from the scene: **the
	// half-space the oblique clip keeps has to contain the middle of the room the
	// hole leads to.** A hole aimed outward keeps the half-space on the other
	// side, so the room it names is behind the camera's clip plane and the sign
	// flips.
	//
	// This scene makes that easy to get wrong in a second way, which is why the
	// case ends with a body rather than a camera: its two panes are
	// perpendicular, so the destination carries a quarter turn, and a turn is
	// the one part of a pairing that can be right for the picture and backwards
	// for the walk.
	const StagedAssets assets;

	Store store("portals");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Portals-1-world.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The middles of the two rooms the pair joins. The third - the library -
	// has no hole in it, because an ordinary doorway is what the walk uses to
	// get there and that is the contrast the scene is built on.
	const engine::core::Vector3 HALL{-20.0f, 7.5f, -20.0f};
	const engine::core::Vector3 GARDEN{20.0f, 7.5f, 20.0f};

	struct Hole {
		const char *Pane;
		const char *Destination;

		// The middle of the room this hole leads to, and whether the scene's own
		// viewer stands on the side of the pane that shows it.
		//
		// **Only one of a pair can be checked from one eye**, and that is a fact
		// about holes rather than a gap in the test: the two panes are in two
		// rooms, the surface pass places each from whichever side the viewer is
		// on, and a pane seen from behind keeps the other half-space. The scene
		// opens outside the west wall, which is in front of the hall's pane and
		// behind the garden's.
		engine::core::Vector3 Room;
		bool Facing;
	};
	const Hole holes[] = {
		{"HallSouth", "GardenWest", GARDEN, true},
		{"GardenWest", "HallSouth", HALL, false},
	};

	// Found by component rather than by name, so renaming a portal in the scene
	// is not a test failure - what it leads to is.
	const auto portalOn = [&store](Entity pane) {
		Entity found = engine::ecs::NULL_ENTITY;
		store.EachChild(pane, [&](Entity child) {
			if (found == engine::ecs::NULL_ENTITY && store.Get<engine::scene::Portal>(child) != nullptr) {
				found = child;
			}
		});
		return found;
	};

	for (const Hole &hole : holes) {
		INFO(hole.Pane);

		const Entity pane = InScene(store, hole.Pane);
		REQUIRE(pane != engine::ecs::NULL_ENTITY);

		const Entity portal = portalOn(pane);
		REQUIRE(portal != engine::ecs::NULL_ENTITY);

		// **A `Portal` is a `SurfaceCamera`**, which is what makes it cost the
		// mirror's path and nothing more.
		CHECK(store.Get<SurfaceCamera>(portal) != nullptr);
		CHECK(store.Get<engine::scene::Portal>(portal)->Destination == InScene(store, hole.Destination));
	}

	REQUIRE(engine::scene::AimSurfaceCameras(store) == 2);

	const auto *viewer = store.Get<engine::scene::Transform>(InScene(store, "Viewer"));
	REQUIRE(viewer != nullptr);

	bool seen[engine::scene::MAX_SURFACES] = {};

	for (const Hole &hole : holes) {
		INFO(hole.Pane);

		const Entity pane = InScene(store, hole.Pane);
		const Entity portal = portalOn(pane);

		// A target each, as `MAX_SURFACES` allows sixteen of.
		const int16_t index = store.Get<SurfaceCamera>(portal)->Surface;
		REQUIRE(index >= 0);
		REQUIRE(static_cast<size_t>(index) < engine::scene::MAX_SURFACES);
		CHECK(store.Get<Visual>(pane)->Surface == index);
		CHECK_FALSE(seen[index]);
		seen[index] = true;

		const auto *lens = store.Get<engine::scene::SurfaceLens>(portal);
		REQUIRE(lens != nullptr);

		// **The oblique clip is doing work, which a mirror's cannot show.** A
		// portal's destination sits in a wall, so the plane is what stops the far
		// side of that wall drawing over the whole hole.
		REQUIRE(lens->ClipNormal.Magnitude() > 0.5f);

		if (!hole.Facing) {
			// Aimed from an eye behind this pane. Nothing about the half-space it
			// keeps is meaningful, so it is only here to be counted.
			continue;
		}

		// The room the hole leads to is in front of the clip plane.
		const float room = lens->ClipNormal.Dot(hole.Room) - lens->ClipDistance;
		INFO("clip half-space at the far room's middle: " << room);
		CHECK(room > 0.0f);

		const auto &placed = store.Get<engine::scene::Transform>(portal)->Frame;

		// And the camera is outside that room looking into it, rather than
		// standing in it looking out - the other half of the same mistake.
		CHECK(lens->ClipNormal.Dot(placed.Position) - lens->ClipDistance < 0.0f);
		CHECK(placed.LookVector().Dot(lens->ClipNormal) > 0.9f);

		// **Rigid, which is what separates a portal from a badly scaled one.**
		// The map is a rotation and a translation, so the camera stands as far
		// from the destination's face as the eye does from the source's - and
		// that is measured here rather than assumed, because a scale slipped into
		// the matrix would leave every other check in this case passing.
		const auto *source = store.Get<engine::scene::Transform>(pane);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane);
		REQUIRE(source != nullptr);
		REQUIRE(bounds != nullptr);

		// **Rotated into the world, because one pane in this scene is turned.**
		// The hall's south wall carries the quarter turn that makes the corner,
		// so `NormalOf(Face)` is the axis in the *part's* frame and only the
		// reach is measurable there - a test that treated the local normal as the
		// world one would measure the wrong gap on exactly the pane the scene
		// exists to demonstrate.
		const engine::core::Vector3 local = engine::scene::NormalOf(store.Get<SurfaceCamera>(portal)->Face);
		const engine::core::Vector3 normal = source->Frame.VectorToWorldSpace(local).Unit();
		const engine::core::Vector3 reach{
			std::abs(local.X) * bounds->HalfExtent.X,
			std::abs(local.Y) * bounds->HalfExtent.Y,
			std::abs(local.Z) * bounds->HalfExtent.Z,
		};

		const float eyeToFace =
			std::abs((viewer->Frame.Position - source->Frame.Position).Dot(normal)) - reach.Magnitude();
		const float cameraToFarFace = std::abs(lens->ClipNormal.Dot(placed.Position) - lens->ClipDistance);
		CHECK(cameraToFarFace == Approx(eyeToFace).margin(1e-2f));
	}

	// Motion on the far side of every hole, six per room. A still portal is
	// indistinguishable from a painted mural.
	CHECK(CountNamed(store, "Drifter") == 18);

	// **And the walk closes, which is the claim the pictures cannot make.**
	// `scene/tests/SurfaceCameras.cpp` proves `CrossPortals` maps a body through
	// the same matrix as the camera; what is scene-specific - and what a pair of
	// perpendicular panes can get backwards while every image still looks right
	// - is *which way round* the two ends are glued. Walk west out of the garden
	// and the hall has to arrive ahead of you, not behind or beside you.
	//
	// It is also what pins the rails camera in the scene file: those legs are
	// this arithmetic, written out by hand because a script has no `Inverse`.
	const Entity walker = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Walker");

	// **Watched by a camera, because a player is a body and an eye.** The yaw is
	// where a player's view direction actually lives, so a pair that turns a
	// corner has to turn it - a body that comes out walking north under a camera
	// still pointing west is the view snapping to a wall on the frame you cross,
	// and W walking you sideways from then on. West is a yaw of a quarter turn
	// under `PlaceCamera`'s convention.
	engine::scene::CameraController watching;
	watching.Subject = walker;
	watching.Angles = engine::core::Vector2{0.0f, std::numbers::pi_v<float> / 2.0f};
	store.SetResource(watching);

	store.Set<engine::scene::Transform>(
		walker, engine::scene::Transform{engine::core::CFrame(engine::core::Vector3{0.0f, 6.0f, 20.0f})}
	);
	store.Set<engine::scene::PreviousTransform>(
		walker,
		engine::scene::PreviousTransform{engine::core::CFrame(engine::core::Vector3{3.0f, 6.0f, 20.0f})}
	);
	store.Set<engine::scene::Motion>(
		walker, engine::scene::Motion{engine::core::Vector3{-16.0f, 0.0f, 0.0f}, engine::core::Vector3::Zero}
	);

	REQUIRE(engine::scene::CrossPortals(store) == 1);

	// **As far past the hall's face as it went past the garden's**, at the
	// height it left at, and at the middle of the wall because that is where it
	// crossed. The panes are a quarter of a metre thick, so a body that stepped
	// to the middle of one steps out an eighth past the other - the thinness is
	// the scene's, and it is what stops "inside the pane" being somewhere a
	// character can stand.
	//
	// **And exactly there, because `scene`'s `LANDING_CLEARANCE` is a floor
	// rather than an offset.** A crosser whose step already ended clear of the
	// plane gets nothing added - adding it unconditionally moved a body a little
	// on every crossing, so walking through a hole and back landed you beside
	// where you started.
	const engine::core::Vector3 landed = store.Get<engine::scene::Transform>(walker)->Frame.Position;
	CHECK(landed.X == Approx(-20.0f).margin(1e-3f));
	CHECK(landed.Y == Approx(6.0f).margin(1e-3f));
	CHECK(landed.Z == Approx(-0.25f).margin(1e-3f));

	// **Turned with it, at the speed it had.** West became north, which is the
	// quarter turn the building is missing. A pair glued the other way round
	// sends this one south, back into the wall it came out of.
	const engine::core::Vector3 speed = store.Get<engine::scene::Motion>(walker)->Linear;
	CHECK(speed.X == Approx(0.0f).margin(1e-3f));
	CHECK(speed.Z == Approx(-16.0f).margin(1e-3f));

	// **And the eye turns with it - on the machine the eye is on.** The
	// crossing writes `scene::PortalTransit` on the body rather than reaching
	// for a camera, because the host that moves a character and the host that
	// draws for its player are two different worlds the moment a server is
	// involved. `FollowPortalTransit` is the other end, and running it here is
	// what a client's camera pass does every frame.
	CHECK(store.Get<engine::scene::PortalTransit>(walker)->Serial == 1u);
	CHECK(engine::scene::FollowPortalTransit(store));

	// A yaw of zero is north, which is the way the walk carries on.
	CHECK(store.Resource<engine::scene::CameraController>()->Angles.Y == Approx(0.0f).margin(1e-3f));

	// **Once, however many times it is asked.** A camera that turned again on
	// the next frame would spin a quarter turn per frame for ever.
	CHECK_FALSE(engine::scene::FollowPortalTransit(store));
	CHECK(store.Resource<engine::scene::CameraController>()->Angles.Y == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("the tunnels scene is shorter and longer inside than out", "[examples][scene]") {
	// **The claim this scene makes is a pair of lengths, so the lengths are what
	// is asserted.** Every other portal example can be checked by asking whether
	// a hole renders; this one is only interesting if walking through a
	// thirty-two stud building takes four studs and walking through a four stud
	// one takes twenty-eight. Both are `CrossPortals` arithmetic against panes
	// the script placed, which is exactly what a wrongly aimed pane breaks.
	//
	// It is also the one arrangement the other portal scenes do not have: panes
	// part-way *down* a corridor rather than filling a doorway, two of them back
	// to back a stud apart, and a long interior isolated from both visible
	// shells.
	const StagedAssets assets;

	Store store("tunnels");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Tunnels.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	const auto zOf = [&store](const char *name) {
		const Entity found = InScene(store, name);
		REQUIRE(found != engine::ecs::NULL_ENTITY);
		return store.Get<engine::scene::Transform>(found)->Frame.Position.Z;
	};

	// The two shells, end to end. These are what an eye on the plain measures.
	CHECK(store.Get<engine::scene::Bounds>(InScene(store, "LongFloor"))->HalfExtent.Z == Approx(16.0f));
	CHECK(store.Get<engine::scene::Bounds>(InScene(store, "ShortFloor"))->HalfExtent.Z == Approx(2.0f));

	// And the panes, which are what a body measures. The west tunnel's walk is
	// its two stubs; the east tunnel's is its two studs plus its isolated
	// interior.
	const float westWalk = (16.0f - zOf("LongSkipNorth")) * 2.0f;
	const float eastWalk = (2.0f - zOf("ShortNorth")) * 2.0f + zOf("ShortInteriorNorth") * 2.0f;

	INFO("west walk " << westWalk << ", east walk " << eastWalk);
	CHECK(westWalk == Approx(4.0f));
	CHECK(eastWalk == Approx(28.0f));

	// Something moving in each, because a still portal is indistinguishable from
	// a painted mural - and because the timing of a crossing is the half of the
	// illusion a screenshot cannot carry.
	CHECK(CountNamed(store, "LongDrifter") == 1);
	CHECK(CountNamed(store, "ShortDrifter") == 1);

	// **Three pairs, six holes, each naming the other back.** A pane whose
	// partner did not name it is a mirror, and a mirror in either of these
	// tunnels is a wall across the walk.
	//
	// Checked by component rather than by aiming, because aiming needs an eye
	// and this scene deliberately has no camera in it.
	const auto portalOn = [&store](const char *name) {
		const Entity pane = InScene(store, name);
		REQUIRE(pane != engine::ecs::NULL_ENTITY);

		Entity found = engine::ecs::NULL_ENTITY;
		store.EachChild(pane, [&](Entity child) {
			if (found == engine::ecs::NULL_ENTITY && store.Get<engine::scene::Portal>(child) != nullptr) {
				found = child;
			}
		});
		REQUIRE(found != engine::ecs::NULL_ENTITY);
		return store.Get<engine::scene::Portal>(found)->Destination;
	};

	const char *const pairs[][2] = {
		{"LongSkipNorth", "LongSkipSouth"},
		{"ShortNorth", "ShortInteriorNorth"},
		{"ShortSouth", "ShortInteriorSouth"},
	};

	for (const auto &pair : pairs) {
		INFO(pair[0] << " <-> " << pair[1]);
		CHECK(portalOn(pair[0]) == InScene(store, pair[1]));
		CHECK(portalOn(pair[1]) == InScene(store, pair[0]));
	}

	// A body walking one step, put down where the step ended.
	//
	// **The seam is the pane's *face*, not its middle** - `GatherSeams` pushes
	// the centre out by the reach along the normal - so a pane a quarter thick
	// standing at `z` has its plane an eighth beyond that. Each step below ends
	// five eighths past its plane and therefore lands five eighths past the far
	// one: `LANDING_CLEARANCE` is a floor rather than an offset, and a step that
	// already cleared it gets nothing added.
	//
	// Every walk is on the tunnel's centre line, so the half-turn in the map has
	// no transverse offset to flip and the landing is a pure translation.
	const Entity walker = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Walker");

	const auto step = [&store, walker](engine::core::Vector3 from, engine::core::Vector3 to) {
		store.Set<engine::scene::PreviousTransform>(
			walker, engine::scene::PreviousTransform{engine::core::CFrame(from)}
		);
		store.Set<engine::scene::Transform>(walker, engine::scene::Transform{engine::core::CFrame(to)});
		store.Set<engine::scene::Motion>(
			walker, engine::scene::Motion{to - from, engine::core::Vector3::Zero}
		);

		REQUIRE(engine::scene::CrossPortals(store) == 1);
		return store.Get<engine::scene::Transform>(walker)->Frame.Position;
	};

	// **Long outside, short inside.** Two studs into a thirty-two stud building
	// and the walk is already at the far end's last two studs.
	const engine::core::Vector3 skipped = step({-20.0f, 4.0f, 14.5f}, {-20.0f, 4.0f, 13.5f});
	CHECK(skipped.X == Approx(-20.0f).margin(1e-3f));
	CHECK(skipped.Y == Approx(4.0f).margin(1e-3f));
	CHECK(skipped.Z == Approx(-14.75f).margin(1e-3f));

	// **Short outside, long inside, and it lands in its own subspace.** One stud
	// into a four stud box arrives in twenty-six studs of isolated corridor.
	const engine::core::Vector3 entered = step({20.0f, 4.0f, 1.5f}, {20.0f, 4.0f, 0.5f});
	CHECK(entered.X == Approx(256.0f).margin(1e-3f));
	CHECK(entered.Y == Approx(4.0f).margin(1e-3f));
	CHECK(entered.Z == Approx(12.25f).margin(1e-3f));

	// And out the far end of the box it never left.
	const engine::core::Vector3 left = step({256.0f, 4.0f, -12.5f}, {256.0f, 4.0f, -13.5f});
	CHECK(left.X == Approx(20.0f).margin(1e-3f));
	CHECK(left.Y == Approx(4.0f).margin(1e-3f));
	CHECK(left.Z == Approx(-1.75f).margin(1e-3f));

	// The long tunnel no longer lends physical space to the short one.
	CHECK(InScene(store, "MiddleNorth") == engine::ecs::NULL_ENTITY);
	CHECK(InScene(store, "MiddleSouth") == engine::ecs::NULL_ENTITY);
	const Entity interiorFloor = InScene(store, "ShortInteriorFloor");
	const Entity interiorCeiling = InScene(store, "ShortInteriorCeiling");
	const Entity interiorPane = InScene(store, "ShortInteriorNorth");
	REQUIRE(interiorFloor != engine::ecs::NULL_ENTITY);
	REQUIRE(interiorCeiling != engine::ecs::NULL_ENTITY);
	REQUIRE(interiorPane != engine::ecs::NULL_ENTITY);
	const auto *floorPlacement = store.Get<engine::scene::Transform>(interiorFloor);
	const auto *floorBounds = store.Get<engine::scene::Bounds>(interiorFloor);
	const auto *ceilingPlacement = store.Get<engine::scene::Transform>(interiorCeiling);
	const auto *ceilingBounds = store.Get<engine::scene::Bounds>(interiorCeiling);
	const auto *panePlacement = store.Get<engine::scene::Transform>(interiorPane);
	const auto *paneBounds = store.Get<engine::scene::Bounds>(interiorPane);
	REQUIRE(floorPlacement != nullptr);
	REQUIRE(floorBounds != nullptr);
	REQUIRE(ceilingPlacement != nullptr);
	REQUIRE(ceilingBounds != nullptr);
	REQUIRE(panePlacement != nullptr);
	REQUIRE(paneBounds != nullptr);
	CHECK(floorPlacement->Frame.Position.Y + floorBounds->HalfExtent.Y == Approx(0.0f));
	CHECK(ceilingPlacement->Frame.Position.Y - ceilingBounds->HalfExtent.Y == Approx(8.0f));
	CHECK(panePlacement->Frame.Position.Y - paneBounds->HalfExtent.Y == Approx(0.0f));
	CHECK(panePlacement->Frame.Position.Y + paneBounds->HalfExtent.Y == Approx(8.0f));

	// **Nothing is set as the world's camera**, which is what makes this one
	// walkable where `Hallway.luau` is a capture: a `CurrentCamera` standing in
	// a world somebody presses Play in overrides the character's own.
	CHECK(InScene(store, "Viewer") == engine::ecs::NULL_ENTITY);
}

TEST_CASE("the tunnels scene leaves its walk paths clear", "[examples][scene]") {
	// **A demonstration you cannot walk down demonstrates nothing**, and this
	// one was blocked by its own props: the two drifting blocks travelled each
	// tunnel's centre line at eye height, so a block stood in the mouth on the
	// approach and was the first thing in the picture when a pane was looked
	// through. The isolated corridor - the one space in the scene that only
	// exists on the far side of a hole - had one running its whole length.
	//
	// So the claim is a *volume*: down the middle of every walked space, from
	// above the floor markings to over a character's head, nothing stands.
	// `DRIFT_SIDE` is what the scene answers with, and this is what stops the
	// answer being quietly reverted.
	//
	// **The panes are the one exception and they are excluded by construction,
	// not by name.** A portal fills its cross-section - that is what makes it a
	// hole rather than a window with a frame - and it is authored
	// `CanCollide = false` so a walker passes through it. Anything else in the
	// channel is furniture in a doorway.
	//
	// What a headless run cannot decide is what the tunnel *looks* like, and
	// `scripts/demos/capture-tunnels.sh` is the half that can: it photographs
	// the three authored viewpoints and counts the blocking pixels.
	const StagedAssets assets;

	Store store("tunnels.walk");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Tunnels.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// One tick, so the drifters have been placed by their own `Heartbeat` rather
	// than left at the birthplace `block` gave them. A route's first leg starts
	// out on the plain, so an unticked scene would be measured before the props
	// had moved at all.
	systems.Tick(store, 1.0f / 60.0f);

	// The walked channel, in metres. Two studs across is a body's width with
	// room either side; the floor stops at 0.5 so the painted stripes - which
	// are 0.35 tall and are the floor - are not read as obstacles; the ceiling
	// stops at `CHARACTER_HEIGHT` so the lamps hanging at 7.3 are not either.
	constexpr float CHANNEL_HALF_WIDTH = 1.0f;
	constexpr float CHANNEL_FLOOR = 0.5f;
	constexpr float CHANNEL_CEILING = 5.0f;

	struct Corridor {
		const char *What;
		float X;
		float FromZ;
		float ToZ;
	};

	// Every space in this scene a body is meant to walk, including the six studs
	// of plain either side of each mouth - a prop parked outside a doorway
	// blocks the approach as surely as one inside it.
	const std::array<Corridor, 3> corridors{
		Corridor{"the west tunnel", -20.0f, -22.0f, 22.0f},
		Corridor{"the east tunnel", 20.0f, -8.0f, 8.0f},
		Corridor{"the isolated interior", 256.0f, -13.0f, 13.0f},
	};

	// Whether this part is a portal pane. Asked of the part rather than of its
	// name, because a pane is a part with a `Portal` on a child of it and that
	// is what every one of the six is.
	const auto isPane = [&store](Entity part) {
		bool found = false;
		store.EachChild(part, [&](Entity child) {
			found = found || store.Get<engine::scene::Portal>(child) != nullptr;
		});
		return found;
	};

	size_t measured = 0;
	store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
		[&](Entity part, const engine::scene::Transform &placement, const engine::scene::Bounds &bounds) {
			if (isPane(part)) {
				return;
			}

			// A conservative world box: the rotated half-extent along each
			// world axis. Every part in this scene is either unrotated or
			// turned half a lap about Y, so this is exact for all of them - and
			// it stays honest if one is ever tilted.
			const engine::core::Vector3 right = placement.Frame.RightVector() * bounds.HalfExtent.X;
			const engine::core::Vector3 up = placement.Frame.UpVector() * bounds.HalfExtent.Y;
			const engine::core::Vector3 ahead = placement.Frame.LookVector() * bounds.HalfExtent.Z;
			const engine::core::Vector3 reach{
				std::abs(right.X) + std::abs(up.X) + std::abs(ahead.X),
				std::abs(right.Y) + std::abs(up.Y) + std::abs(ahead.Y),
				std::abs(right.Z) + std::abs(up.Z) + std::abs(ahead.Z),
			};
			const engine::core::Vector3 at = placement.Frame.Position;

			for (const Corridor &corridor : corridors) {
				const bool acrossX = std::abs(at.X - corridor.X) < CHANNEL_HALF_WIDTH + reach.X;
				const bool throughY = at.Y + reach.Y > CHANNEL_FLOOR && at.Y - reach.Y < CHANNEL_CEILING;
				const bool alongZ = at.Z + reach.Z > corridor.FromZ && at.Z - reach.Z < corridor.ToZ;

				if (acrossX && throughY && alongZ) {
					INFO(
						"'" << store.InstanceNameOf(part).Text() << "' stands in " << corridor.What << " at ("
							<< at.X << ", " << at.Y << ", " << at.Z << ")"
					);
					CHECK(false);
				}
			}
			measured++;
		}
	);

	// A scene that loaded nothing would pass every line above. The floors,
	// ceilings, walls, stripes, lamps, posts and drifters are forty-odd parts;
	// this only has to be more than none.
	CHECK(measured > 20);

	// And the props are still there rather than deleted, which is the other way
	// to make the channels clear and is not the fix.
	CHECK(CountNamed(store, "LongDrifter") == 1);
	CHECK(CountNamed(store, "ShortDrifter") == 1);
}

TEST_CASE("the interface scene builds and connects its buttons", "[examples][scene][gui]") {
	const StagedAssets assets;

	Store store("interface");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Interface.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountElements(store, "Confirm") == 1);
	CHECK(CountElements(store, "Cancel") == 1);
	for (int index = 1; index <= 6; index++) {
		CHECK(CountElements(store, "Swatch" + std::to_string(index)) == 1);
	}

	const Entity hint = FirstElement(store, "Hint");
	REQUIRE(hint != engine::ecs::NULL_ENTITY);
	const engine::gui::Label *label = store.Get<engine::gui::Label>(hint);
	REQUIRE(label != nullptr);
	CHECK(label->Text.find("0 clicks") != std::string::npos);

	engine::gui::Screen display;
	display.Width = 1920.0f;
	display.Height = 1080.0f;

	const size_t rendered = engine::gui::Layout(store, display);
	CHECK(rendered > 0);

	const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(hint);
	REQUIRE(placed != nullptr);
	CHECK(placed->Rendered);
	CHECK(placed->AbsoluteSize.X > 0.0f);

	const Entity close = FirstElement(store, "Close");
	REQUIRE(close != engine::ecs::NULL_ENTITY);

	Entity screen = engine::ecs::NULL_ENTITY;
	store.Each<const engine::gui::Layer>([&](Entity entity, const engine::gui::Layer &) {
		if (screen == engine::ecs::NULL_ENTITY &&
			store.InstanceNameOf(entity) == engine::core::Name("InterfaceExample")) {
			screen = entity;
		}
	});
	REQUIRE(screen != engine::ecs::NULL_ENTITY);

	engine::gui::Layer *layer = store.GetMutable<engine::gui::Layer>(screen);
	REQUIRE(layer != nullptr);
	CHECK(layer->Enabled);

	layer->Enabled = false;
	CHECK(engine::gui::Layout(store, display) == 0);
	CHECK_FALSE(store.Get<engine::gui::Resolved>(hint)->Rendered);
}

TEST_CASE("the world interface scene contains every collector and a nested scene", "[examples][scene][gui]") {
	const StagedAssets assets;
	Store store("interface_world");
	Scheduler systems;

	std::string error;
	INFO(error);
	REQUIRE(LoadScene(store, systems, ExamplePath("InterfaceWorld.luau"), error));

	const Entity pane = InScene(store, "SurfacePanel");
	const Entity marker = InScene(store, "BillboardMarker");
	const Entity previewPane = InScene(store, "ViewportPanel");
	REQUIRE(pane != engine::ecs::NULL_ENTITY);
	REQUIRE(marker != engine::ecs::NULL_ENTITY);
	REQUIRE(previewPane != engine::ecs::NULL_ENTITY);

	const Entity surface = store.FindFirstChild(pane, "WorldControls");
	const Entity billboard = store.FindFirstChild(marker, "MarkerLabel");
	const Entity previewSurface = store.FindFirstChild(previewPane, "ViewportSurface");
	const Entity viewport = store.FindFirstChild(previewSurface, "NestedScene");
	REQUIRE(surface != engine::ecs::NULL_ENTITY);
	REQUIRE(billboard != engine::ecs::NULL_ENTITY);
	REQUIRE(viewport != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::gui::Surface>(surface) != nullptr);
	CHECK(store.Get<engine::gui::Billboard>(billboard) != nullptr);

	const engine::gui::Viewport *scene = store.Get<engine::gui::Viewport>(viewport);
	REQUIRE(scene != nullptr);
	CHECK(store.Get<engine::scene::Camera>(scene->CurrentCamera) != nullptr);
	CHECK(store.FindFirstChild(viewport, "PreviewFloor") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(viewport, "PreviewCube") != engine::ecs::NULL_ENTITY);

	engine::gui::Screen display;
	display.Width = 1280.0f;
	display.Height = 720.0f;
	CHECK(engine::gui::Layout(store, display) > 0);
	const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(viewport);
	REQUIRE(placed != nullptr);
	CHECK(placed->Rendered);
	CHECK(placed->AbsoluteSize.X == Approx(640.0f));
	CHECK(placed->AbsoluteSize.Y == Approx(360.0f));
}

TEST_CASE("the four-world mirrors scene varies by world", "[examples][scene][worlds]") {
	const StagedAssets assets;

	const auto casterCount = [&](const char *worldName) {
		Store store(worldName);
		Scheduler systems;

		std::string error;
		const bool loaded = LoadScene(store, systems, ExamplePath("Mirrors-4-worlds.luau"), error);
		INFO(error);
		REQUIRE(loaded);

		CHECK(CountNamed(store, "Baseplate") == 1);
		CHECK(CountNamed(store, "Mirror") == 1);

		size_t surfaces = 0;
		store.Each<const SurfaceCamera>([&](Entity, const SurfaceCamera &) { surfaces++; });
		CHECK(surfaces == 1);

		return CountNamed(store, "Caster");
	};

	CHECK(casterCount("client.world") == 6);
	CHECK(casterCount("client.world.1") == 9);
	CHECK(casterCount("client.world.2") == 12);
	CHECK(casterCount("client.world.3") == 15);

	CHECK(casterCount("client.world.4") == 6);
}

TEST_CASE("the four-world scene builds the same way in TypeScript", "[examples][scene][worlds][js]") {
	const StagedAssets assets;

	const std::filesystem::path transpiled = ExamplePath("Mirrors-4-worlds.js");
	if (!std::filesystem::exists(transpiled)) {
		SUCCEED("no transpiled twin; tsc was not available at configure time");
		return;
	}

	Store store("client.world.2");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, transpiled.string(), error);
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountNamed(store, "Baseplate") == 1);
	CHECK(CountNamed(store, "Mirror") == 1);
	CHECK(CountNamed(store, "Caster") == 12);
}

TEST_CASE("the gui containment names match the services scene installs", "[examples][scene][gui]") {
	Store store("examples_test.gui_names");
	engine::scene::InstallServices(store);

	CHECK(store.FindFirstRoot(engine::gui::WORKSPACE) != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstRoot(engine::gui::STARTER_GUI) != engine::ecs::NULL_ENTITY);

	CHECK(engine::gui::PLAYER_GUI == engine::scene::PLAYER_GUI_NAME);

	const Entity player = engine::scene::AddPlayer(store, "Someone", true);
	REQUIRE(player != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(player, engine::gui::PLAYER_GUI) != engine::ecs::NULL_ENTITY);
}

TEST_CASE("the studio's TypeScript property grid builds its tree", "[examples][scene][gui][panel]") {
	// **The point of the whole 2D branch, and deliberately the last step.**
	// `mono.studio` keeps Dear ImGui until the engine's own tree can draw a
	// property grid - because an editor half on each is two widget sets - so
	// this is not a replacement for the imgui panels. It is the proof that the
	// tree can carry one, which is what has to be true before any of them move.
	//
	// **Loaded as `.js`, because that is what the toolchain produced.**
	// `mono.studio/panels/*.ts` is transpiled at staging, the same way the
	// examples are and for the reason `Runtime.hpp` has always given: the engine
	// loads what a toolchain emitted and compiles no TypeScript itself.
	//
	// What it exercises, and none of it had a caller before this version:
	// `UIListLayout` stacking rows, `UIPadding` insetting them, a
	// `ScrollingFrame` whose canvas is longer than its panel, `.Activated` on a
	// generated row - the `gui`-to-`script` join - and `StarterGui` containment,
	// without which the whole thing draws nothing.
	const StagedAssets assets;

	const std::filesystem::path panel = engine::core::Paths::Assets() / "panels" / "Properties.js";
	if (!std::filesystem::exists(panel)) {
		SUCCEED("no transpiled panel; tsc was not available at configure time");
		return;
	}

	Store store("studio.panel");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, panel.string(), error);
	INFO(error);
	REQUIRE(loaded);

	// Ten rows, each a `TextButton` with a key and a value label under it. The
	// count is asserted so that a panel which silently built nothing - the
	// failure a containment or layout regression produces - cannot pass.
	CHECK(CountElements(store, "Position") == 1);
	CHECK(CountElements(store, "Transparency") == 1);
	CHECK(CountElements(store, "Key") == 10);
	CHECK(CountElements(store, "Value") == 10);

	// **And it lays out**, which is the claim the tree existing does not make.
	engine::gui::Screen display;
	display.Width = 1920.0f;
	display.Height = 1080.0f;
	CHECK(engine::gui::Layout(store, display) > 0);

	const Entity rows = FirstElement(store, "Rows");
	REQUIRE(rows != engine::ecs::NULL_ENTITY);

	const engine::gui::Resolved *placed = store.Get<engine::gui::Resolved>(rows);
	REQUIRE(placed != nullptr);
	CHECK(placed->Rendered);

	// The panel is anchored to the right edge, so its rows sit in the right half
	// of a 1920-wide screen. A layout that ignored `AnchorPoint` would put them
	// at the far right *edge* rather than inset from it, and one that ignored
	// `Position` would put them at zero - this separates all three.
	CHECK(placed->AbsolutePosition.X > 960.0f);
	CHECK(placed->AbsolutePosition.X < 1920.0f - 300.0f);
}

namespace {

	// Every terrain chunk the scene built, as a canonical sorted list of
	// vertex positions.
	//
	// Sorted rather than taken in iteration order, because two worlds built the
	// same way are only guaranteed to hold the same *set* of rows - an archetype
	// walk is free to visit them in a different sequence, and a determinism
	// check that compared sequences would fail for a reason that has nothing to
	// do with the generator.
	std::vector<std::array<float, 3>> TerrainVertices(Store &store) {
		std::vector<std::array<float, 3>> points;

		store.Each<const engine::scene::EditableMesh>([&](Entity, const engine::scene::EditableMesh &mesh) {
			for (const engine::core::Vector3 &at : mesh.Positions) {
				points.push_back({at.X, at.Y, at.Z});
			}
		});

		std::sort(points.begin(), points.end());
		return points;
	}

	// How many chunk parts the scene has put in the world so far.
	size_t TerrainChunks(Store &store) {
		size_t chunks = 0;
		store.Each<const engine::scene::Transform, const Visual>(
			[&](Entity entity, const engine::scene::Transform &, const Visual &) {
				if (store.InstanceNameOf(entity).Text().rfind("Terrain_", 0) == 0) {
					chunks++;
				}
			}
		);
		return chunks;
	}

	// Ticks until the scene has finished building, or gives up.
	//
	// **The scene builds a chunk a frame rather than all of them before the
	// first present**, so a test that only loaded it would find an empty world -
	// which is the scene working as its header describes rather than a failure.
	// The bound is generous and the loop stops as soon as the count settles.
	void BuildTerrain(Store &store, Scheduler &systems, size_t expected) {
		for (int tick = 0; tick < 400 && TerrainChunks(store) < expected; tick++) {
			systems.Tick(store, 1.0f / 60.0f);
		}
	}
}

TEST_CASE("the terrain scene builds a coloured heightfield mesh", "[examples][scene]") {
	const StagedAssets assets;

	Store store("terrain");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Terrain.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// 512 studs cut into 64-stud chunks is 8 by 8 of them.
	constexpr size_t CHUNKS = 64;
	BuildTerrain(store, systems, CHUNKS);
	CHECK(TerrainChunks(store) == CHUNKS);

	// **One `EditableMesh` per chunk and not one shared between them**, which is
	// what makes each a `MeshPart` the frustum can reject on its own. A scene
	// that built one enormous mesh would pass a part count and fail here.
	size_t meshes = 0;
	size_t vertices = 0;
	size_t indices = 0;
	store.Each<const engine::scene::EditableMesh>([&](Entity, const engine::scene::EditableMesh &mesh) {
		meshes++;
		vertices += mesh.Positions.size();
		indices += mesh.Indices.size();
	});

	CHECK(meshes == CHUNKS);

	// A chunk is 65 by 65 samples - the extra column and row are the ones it
	// shares with its neighbours, which is what makes the surface continuous -
	// and 64 by 64 cells of two triangles each.
	CHECK(vertices == CHUNKS * 65 * 65);
	CHECK(indices == CHUNKS * 64 * 64 * 2 * 3);

	// **More than one colour, which is the whole of the "color them" ask.** The
	// bands are cut from the field that was generated rather than from constants
	// beside it, so this asserts that the scene painted *something* different
	// somewhere rather than pinning which band a given corner fell in.
	//
	// It is also the only place this suite can see the colours at all: turning
	// them into draw runs is `client::BuildMeshData`'s job and `client` is two
	// tiers above this one.
	size_t distinct = 0;
	store.Each<const engine::scene::EditableMesh>([&](Entity, const engine::scene::EditableMesh &mesh) {
		std::vector<std::array<float, 3>> seen;
		for (const engine::core::Color3 &colour : mesh.Colours) {
			const std::array<float, 3> entry{colour.R, colour.G, colour.B};
			if (std::find(seen.begin(), seen.end(), entry) == seen.end()) {
				seen.push_back(entry);
			}
		}
		distinct = std::max(distinct, seen.size());
	});
	CHECK(distinct > 1);

	// The camera the scene placed, outside the map looking in.
	REQUIRE(store.Resource<ActiveCamera>() != nullptr);

	const Entity eye = InScene(store, "Surveyor");
	REQUIRE(eye != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Transform>(eye)->Frame.Position.Y > 100.0f);

	// Measured bounds, not declared: the whole field is built, so the world
	// reaches the half-width of the map plus whatever the relief adds.
	REQUIRE(store.Resource<WorldBounds>() != nullptr);
	CHECK(store.Resource<WorldBounds>()->HalfExtent > 200.0f);
}

TEST_CASE("the terrain generator is a pure function of its seed", "[examples][scene]") {
	const StagedAssets assets;

	// **Rule 5, asserted rather than asserted-in-a-comment.** Every vertex on
	// screen comes out of `HeightAt`, which means a recording replays if and
	// only if two runs of that function agree. The integer hashing exists for
	// this, and a change that reached for `math.random` or wall time would pass
	// every other check in this file.
	std::vector<std::array<float, 3>> first;
	std::vector<std::array<float, 3>> second;

	for (std::vector<std::array<float, 3>> *into : {&first, &second}) {
		Store store("terrain.determinism");
		Scheduler systems;

		std::string error;
		const bool loaded = LoadScene(store, systems, ExamplePath("Terrain.luau"), error);
		INFO(error);
		REQUIRE(loaded);

		// Four chunks each rather than all sixty-four: the generator is the
		// thing under test and a quarter of a million vertices proves it as
		// well as a million do, at a quarter of the cost.
		for (int tick = 0; tick < 4; tick++) {
			systems.Tick(store, 1.0f / 60.0f);
		}

		*into = TerrainVertices(store);
	}

	REQUIRE(!first.empty());
	REQUIRE(first.size() == second.size());
	CHECK(first == second);
}

// **A world nobody loaded a scene into, which is the editor's shape.**
// `LoadScene` mounts a scene's modules on the way past, so every test below
// proves the mount only for programs that go through it - and `studio::Editor`
// does not: it installs an example as a `Script` instance in a world it built
// itself. A scene whose first line is `require(script.MagicCore)` therefore
// worked under `client --script` and failed in the one program it is authored
// in. This pins the door the editor calls.
TEST_CASE("a scene's modules mount under a script nothing loaded", "[examples][scene]") {
	const StagedAssets assets;

	Store store("libraries.bare");

	engine::scene::RegisterSceneClasses();
	engine::scene::InstallServices(store);

	const Entity holder = store.CreateInstance(engine::script::ScriptClass(), std::string("LibrariesScene"));
	REQUIRE(holder != engine::ecs::NULL_ENTITY);
	REQUIRE(store.FindFirstChild(holder, "MagicCore") == engine::ecs::NULL_ENTITY);

	CHECK(engine::examples::MountSceneLibraries(store, holder, "Libraries.luau") == 2);

	const Entity magic = store.FindFirstChild(holder, "MagicCore");
	REQUIRE(magic != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(magic, "Compiler") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(holder, "TerrainCore") != engine::ecs::NULL_ENTITY);

	// **Nothing in `ReplicatedStorage`, which is the point of the move.** These
	// used to be mirrored there for every world in the program, so a brand-new
	// empty game carried a demo's modules and a scene had no way to say it
	// wanted them.
	const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
	REQUIRE(replicated != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(replicated, "MagicCore") == engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(replicated, "TerrainCore") == engine::ecs::NULL_ENTITY);

	// **Twice is once**, which is what lets a host call it whatever the script's
	// age: a module is cached per instance, so two trees under one script would
	// give `require` two copies that share no state.
	CHECK(engine::examples::MountSceneLibraries(store, holder, "Libraries.luau") == 0);

	size_t named = 0;
	store.EachChild(holder, [&](Entity child) {
		named += store.InstanceNameOf(child) == Name("MagicCore") ? 1 : 0;
	});
	CHECK(named == 1);
}

// A scene with no modules of its own gets none, which is every scene but three.
TEST_CASE("a scene with no modules mounts nothing", "[examples][scene]") {
	const StagedAssets assets;

	Store store("libraries.none");
	engine::scene::RegisterSceneClasses();

	const Entity holder = store.CreateInstance(engine::script::ScriptClass(), std::string("RingsScene"));
	REQUIRE(holder != engine::ecs::NULL_ENTITY);

	CHECK(engine::examples::MountSceneLibraries(store, holder, "Rings.luau") == 0);
}

TEST_CASE("the shipped Luau libraries mount and run", "[examples][scene]") {
	const StagedAssets assets;

	Store store("libraries");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Libraries.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// **Mounted under the script that requires them**, which is Rojo's own
	// arrangement and the contract every `require(script.Parent.X)` inside the
	// ported libraries was written against.
	const Entity holder = InScene(store, "Libraries");
	REQUIRE(holder != engine::ecs::NULL_ENTITY);

	const Entity magic = store.FindFirstChild(holder, "MagicCore");
	REQUIRE(magic != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(magic, "Compiler") != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(magic, "SpellSolver") != engine::ecs::NULL_ENTITY);

	// `Presets/init.luau` collapsed, so the presets are children of the module
	// rather than of a folder beside it. Every preset reaches its library
	// through `script.Parent.Parent`, which only resolves if this is right.
	const Entity presets = store.FindFirstChild(magic, "Presets");
	REQUIRE(presets != engine::ecs::NULL_ENTITY);
	CHECK(store.ClassOf(presets) == engine::script::ModuleScriptClass());
	CHECK(store.FindFirstChild(presets, "ChainLightning") != engine::ecs::NULL_ENTITY);

	const Entity terrain = store.FindFirstChild(holder, "TerrainCore");
	REQUIRE(terrain != engine::ecs::NULL_ENTITY);
	CHECK(store.FindFirstChild(terrain, "Noise") != engine::ecs::NULL_ENTITY);

	// Scene loading reached the generated voxel geometry.
	CHECK(CountNamed(store, "Voxels") > 0);
}

TEST_CASE("the ported libraries pass their own test suite", "[examples][scene]") {
	// The ported libraries run their data tests in this engine's Luau runtime.
	const StagedAssets assets;

	Store store("magic.tests");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("MagicTests.luau"), error);

	// The script reports the first failure in its error message.
	INFO(error);
	REQUIRE(loaded);

	CHECK(CountNamed(store, "MagicTestsPassed") == 1);
	CHECK(CountNamed(store, "MagicTestsFailed") == 0);
}

TEST_CASE("the magic scene fires spells that crater terrain", "[examples][scene]") {
	const StagedAssets assets;

	Store store("magic");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Magic.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The arena exists before the first beat.
	const size_t terrainBefore = CountNamed(store, "Voxels");
	CHECK(terrainBefore > 500);

	// A changed voxel mesh proves the spell reached the world.
	size_t sampled = terrainBefore;
	bool changed = false;
	for (int tick = 0; tick < 60 * 8 && !changed; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
		const size_t now = CountNamed(store, "Voxels");
		if (now != sampled) {
			changed = true;
		}
		sampled = now;
	}
	CHECK(changed);

	// Every lane compiled; skipped lanes would leave an empty arena.
	CHECK(CountNamed(store, "Muzzle") == 5);
}

TEST_CASE("the player list names everybody in the world", "[examples][scene][players]") {
	// **The first scene that reads `Players` at all**, and the assertion is that
	// it reads it *per player*. The panel is built into each player's own
	// `PlayerGui` rather than into `StarterGui`, because a `StarterGui` is a
	// template that `gui::ResetPlayerGui` clones at spawn - a list built there
	// is correct for exactly one instant per player and stale for ever after.
	//
	// This is what a GPU is not needed for: whether the *rows exist* and say the
	// right names is a question about the world, and whether they are painted
	// is the one `AGENTS.md` refuses to answer with a mock renderer.
	const StagedAssets assets;

	Store store("playerlist");
	Scheduler systems;

	// **Players before the script runs**, which is the arrangement a hosted
	// world is actually in: `Server::OnAdmitted` adds a player when a client is
	// admitted, and a scene loaded afterwards has to find whoever is already
	// there rather than waiting for a `PlayerAdded` that has already fired.
	engine::scene::RegisterSceneComponents();
	engine::scene::InstallServices(store);

	const Entity first = engine::scene::AddPlayer(store, "Player1");
	const Entity second = engine::scene::AddPlayer(store, "Player2");
	REQUIRE(first != engine::ecs::NULL_ENTITY);
	REQUIRE(second != engine::ecs::NULL_ENTITY);

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("PlayerList.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// A few ticks, because the panel is finished on the `Heartbeat` that
	// notices a player with no panel - see the scene's last paragraph for why
	// the signal alone is not enough.
	for (int tick = 0; tick < 4; tick++) {
		systems.Tick(store, 1.0f / 60.0f);
	}

	// One panel per player, in that player's own container - not two in one, and
	// not one shared.
	for (const Entity player : {first, second}) {
		INFO(store.InstanceNameOf(player).Text());

		const Entity container = store.FindFirstChild(player, "PlayerGui");
		REQUIRE(container != engine::ecs::NULL_ENTITY);

		const Entity screen = store.FindFirstChild(container, "PlayerList");
		REQUIRE(screen != engine::ecs::NULL_ENTITY);

		const Entity card = store.FindFirstChild(screen, "Card");
		REQUIRE(card != engine::ecs::NULL_ENTITY);

		const Entity rows = store.FindFirstChild(card, "Rows");
		REQUIRE(rows != engine::ecs::NULL_ENTITY);

		// Two players, two rows. **Counted under this player's own panel**,
		// which is what makes the count mean "everybody" rather than "somebody"
		// - a global count of rows named `Row1` would be two whether the second
		// panel held one row or none.
		size_t named = 0;
		store.EachChild(rows, [&](Entity row) {
			const Entity label = store.FindFirstChild(row, "Name");
			if (label == engine::ecs::NULL_ENTITY) {
				return;
			}
			const auto *text = store.Get<engine::gui::Label>(label);
			if (text == nullptr) {
				return;
			}
			if (text->Text == "Player1" || text->Text == "Player2") {
				named++;
			}
		});
		CHECK(named == 2);
	}
}

TEST_CASE("the portal lighting scenes author lamps a seam can carry", "[examples][scene]") {
	// **What a scene gets wrong about portal lighting is placement, and it is
	// silent.** The transport itself is `client::CollectLights`' and
	// `mono.client/tests/PortalLighting.cpp` asserts it; what belongs here is
	// that the two shipped scenes hand that pass what it needs - a linked pair
	// of mouths, every lamp inside its own seam's reach, and a world dark
	// enough that a capture of the far room measures transported light rather
	// than the sun. A lamp authored a stud out of range would load, render,
	// and quietly turn the far room into the dark frame the capture script
	// calls a failure.
	const StagedAssets assets;

	struct Lamp {
		engine::core::Vector3 Position;
		float Range = 0.0f;
	};

	struct Authoring {
		const char *Scene;
		size_t Lamps;
	};

	for (const Authoring &expected : {Authoring{"PortalLightOut.luau", 1}, {"PortalLightMix.luau", 2}}) {
		INFO(expected.Scene);

		Store store("portal-lighting");
		Scheduler systems;

		std::string error;
		const bool loaded = LoadScene(store, systems, ExamplePath(expected.Scene), error);
		INFO(error);
		REQUIRE(loaded);

		// A linked pair, neither mouth crossing worlds, both enabled.
		static thread_local std::vector<engine::scene::PortalSeam> seams;
		REQUIRE(engine::scene::GatherPortalSeams(store, seams) == 2);
		for (const engine::scene::PortalSeam &seam : seams) {
			CHECK(!seam.Crosses);
		}

		// Every lamp within reach of at least one seam, or nothing crosses.
		std::vector<Lamp> lamps;
		store.Each<const engine::scene::Light>([&](Entity bulb, const engine::scene::Light &light) {
			const auto *fitting = store.Get<engine::scene::Transform>(store.ParentOf(bulb));
			REQUIRE(fitting != nullptr);
			lamps.push_back({fitting->Frame.Position, light.Range});
		});
		REQUIRE(lamps.size() == expected.Lamps);

		for (const Lamp &lamp : lamps) {
			float nearest = std::numeric_limits<float>::infinity();
			for (const engine::scene::PortalSeam &seam : seams) {
				nearest = std::min(nearest, engine::scene::SeamDistance(seam, lamp.Position));
			}
			CHECK(nearest < lamp.Range);
		}

		// No sun and no ambient, so the far room's floor answers for the seam
		// alone - the property both capture reports lean on.
		bool dark = false;
		store.Each<const engine::scene::LightingServiceComponent>(
			[&](Entity, const engine::scene::LightingServiceComponent &lighting) {
				dark = lighting.Brightness == 0.0f && lighting.Ambient.R == 0.0f &&
					   lighting.OutdoorAmbient.R == 0.0f;
			}
		);
		CHECK(dark);
	}
}

// The mirror ball, and the two things about it that are arithmetic rather than
// authoring.
//
// **The facets are placed by a formula and nothing else in the repository would
// notice it going wrong.** A tile whose front face points *inward* reflects the
// inside of the ball, which the renderer draws perfectly and which looks exactly
// like a tile that is not reflecting at all - so the sign of `CFrame.lookAt`'s
// look direction is a one-character mistake with no visible symptom and no test
// but this one. `Mirrors-1-world.luau`'s case pins the same property for four
// unrotated walls, where a face is an axis; here every facet is rotated
// differently and the answer has to come out of the geometry.
//
// **And the stated bounce depth is a guard rather than a preference.** A ball is
// the worst shape the automatic rule has: every pane can see most of the others,
// so it says "deeper" at every level and climbs to `render::MAX_SURFACE_DEPTH`,
// where the passes go as `panes x (panes - 1) ^ (levels - 1)` - 3,600 of them at
// sixteen panes. A change that dropped the `workspace.SurfaceBounces = 1` line
// would turn this scene from slow into a hang.
TEST_CASE("the mirror ball mirrors every facet, faces them out and has no holes", "[examples][scene]") {
	// **Two of these claims were false and both read as the mirrors being
	// broken.** The scene authored sixteen cameras over eighty facets because
	// sixteen is what `scene::MAX_SURFACES` used to compile in - so five sixths
	// of a *mirror ball* were not mirrors - and it cut each facet as a square
	// 0.62 of an edge across, which is smaller than the triangle it sits on. A
	// square cannot fill a triangle, so the tiles never met and the ball was a
	// shell of loose plates with the dark core showing at every seam.
	//
	// The budget is the world's since v0.17 and the scene states it. Asking is
	// still the scene's, and every facet asks: what the panes over budget cost
	// is the whole point of a stress scene, so a change that quietly caps the
	// asking again is a regression rather than a saving.
	const StagedAssets assets;

	Store store("mirrorball");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("StressMirrors.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// Two subdivisions of an icosahedron is three hundred and twenty faces, and
	// the count is checked rather than assumed because the midpoint cache is
	// what makes it that - without the sharing every face would still be split
	// into four and the vertex count would be wrong while this number stayed
	// right.
	size_t facets = 0;
	float tiled = 0.0f;
	store.Each<const engine::scene::Transform, const engine::scene::Bounds>(
		[&](Entity part, const engine::scene::Transform &, const engine::scene::Bounds &bounds) {
			if (store.InstanceNameOf(part).Text().rfind("Facet_", 0) != 0) {
				return;
			}
			facets++;

			// The face of the tile that reflects, which is the one a mirror is
			// projected onto. The third axis is the plate's thickness.
			tiled += bounds.HalfExtent.X * 2.0f * bounds.HalfExtent.Y * 2.0f;
		}
	);
	CHECK(facets == 320);

	// **A camera on every one of them, which is what makes it a mirror ball.**
	// The world draws `workspace.MaxSurfaces` of them - the ones covering the
	// most screen - and the rest reach no screen at all; that cost is the finding
	// this scene exists to produce, and capping the asking to hide it would be
	// hiding the finding.
	static thread_local std::vector<engine::scene::SurfacePane> panes;
	REQUIRE(engine::scene::GatherSurfacePanes(store, panes) == facets);

	// **And the tiles more than cover the ball, which is what closes the holes.**
	// A rectangle circumscribing a triangle is twice the triangle's area, so a
	// ball whose every facet is covered costs about twice its own surface in
	// tile; the old squares came to four fifths of it, which is a shell that
	// cannot close however the tiles are turned. The ratio separates the two
	// arrangements without this test re-deriving the ico-sphere and disagreeing
	// with the scene about what it built - the ball's own surface is measured
	// from the core mesh the scene made, which is that ico-sphere scaled down a
	// few per cent to sit under the plates.
	float cored = 0.0f;
	size_t meshes = 0;
	store.Each<const engine::scene::EditableMesh>([&](Entity, const engine::scene::EditableMesh &mesh) {
		meshes++;
		for (size_t at = 0; at + 2 < mesh.Indices.size(); at += 3) {
			const engine::core::Vector3 &a = mesh.Positions[mesh.Indices[at]];
			const engine::core::Vector3 &b = mesh.Positions[mesh.Indices[at + 1]];
			const engine::core::Vector3 &c = mesh.Positions[mesh.Indices[at + 2]];
			cored += (b - a).Cross(c - a).Magnitude() * 0.5f;
		}
	});

	REQUIRE(meshes == 1);
	REQUIRE(cored > 0.0f);

	INFO(tiled << " of tile over " << cored << " of ball");
	CHECK(tiled / cored > 1.8f);

	// The ball's middle, taken from the core rather than from a literal - the
	// scene's `RADIUS` and `HEIGHT` are free to change without this following
	// them.
	const Entity core = InScene(store, "Core");
	REQUIRE(core != engine::ecs::NULL_ENTITY);
	const auto *hub = store.Get<engine::scene::Transform>(core);
	REQUIRE(hub != nullptr);

	for (const engine::scene::SurfacePane &pane : panes) {
		// Every pane on the sphere, facing away from its middle. The radius is
		// not checked against a number for the same reason the centre is not;
		// what matters is that the normal and the outward direction agree.
		const engine::core::Vector3 outward = pane.Centre - hub->Frame.Position;
		CHECK(outward.Magnitude() > 0.0f);
		CHECK(outward.Dot(pane.Normal) > 0.0f);
	}

	CHECK(engine::scene::SurfaceBouncesOf(store) == 1);
}

// What the studio's `World -> New Scene from Example` menu is built from.
//
// **The menu walks the staged directory rather than naming scenes**, so the
// thing that can break is the walk: a listing that comes back empty is a menu
// that says "no staged examples" in a build that staged forty of them, and
// nothing else in the editor would notice. Checked against scenes this suite
// already loads by name, so the two cannot disagree about what is shipped.
TEST_CASE("every staged scene is offered by name, sorted", "[examples][scene]") {
	const StagedAssets assets;

	const std::vector<std::string> scenes = engine::examples::ExampleScenes();
	REQUIRE(!scenes.empty());

	CHECK(std::is_sorted(scenes.begin(), scenes.end()));

	for (const char *shipped :
		 {"Rings.luau", "StressMirrors.luau", "StressParticles.luau", "StressPhysics.luau"}) {
		INFO(shipped);
		CHECK(std::find(scenes.begin(), scenes.end(), shipped) != scenes.end());
	}

	// **Luau only.** A `.ts` scene is staged as transpiled JavaScript under a
	// `.js` name, so offering either would name a second copy of a scene already
	// in the list - and offering the `.ts` itself would name a file no program
	// on this path can read.
	for (const std::string &scene : scenes) {
		INFO(scene);
		CHECK(scene.size() > 5);
		CHECK(scene.compare(scene.size() - 5, 5, ".luau") == 0);
	}
}
