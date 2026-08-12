// The example scenes, loaded.
//
// **A scene that fails to load is one nobody notices is broken** until they run
// the client and see a black screen. Neither the shadow pass nor the surface
// pass can be asserted against without a GPU — `AGENTS.md` names that exception
// and refuses a mock renderer to close it on paper — so what this suite asserts
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
#include <engine/scene/Part.hpp>
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
	// `script/Bindings.hpp`'s `OpenWorkspace` for why the two notions of "the
	// workspace" were collapsed, and `scene/Visibility.hpp` for what the tree
	// now decides.
	//
	// Falls back to a root, because some of these scripts deliberately leave an
	// instance unparented — an orphan is still reachable from C++ through
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
	// across — which is exactly the bug the settle beat in `LoadScene` exists
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

		// **Wide rather than square, and all four the same** — because all four
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

		const int8_t index = store.Get<SurfaceCamera>(reflection)->Surface;
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
	// its room places a camera, fits a frustum, renders — and shows the empty
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

	// The middles of the two rooms the pair joins. The third — the library —
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
	// is not a test failure — what it leads to is.
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
		const int8_t index = store.Get<SurfaceCamera>(portal)->Surface;
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
		// standing in it looking out — the other half of the same mistake.
		CHECK(lens->ClipNormal.Dot(placed.Position) - lens->ClipDistance < 0.0f);
		CHECK(placed.LookVector().Dot(lens->ClipNormal) > 0.9f);

		// **Rigid, which is what separates a portal from a badly scaled one.**
		// The map is a rotation and a translation, so the camera stands as far
		// from the destination's face as the eye does from the source's — and
		// that is measured here rather than assumed, because a scale slipped into
		// the matrix would leave every other check in this case passing.
		const auto *source = store.Get<engine::scene::Transform>(pane);
		const auto *bounds = store.Get<engine::scene::Bounds>(pane);
		REQUIRE(source != nullptr);
		REQUIRE(bounds != nullptr);

		// **Rotated into the world, because one pane in this scene is turned.**
		// The hall's south wall carries the quarter turn that makes the corner,
		// so `NormalOf(Face)` is the axis in the *part's* frame and only the
		// reach is measurable there — a test that treated the local normal as the
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
	// the same matrix as the camera; what is scene-specific — and what a pair of
	// perpendicular panes can get backwards while every image still looks right
	// — is *which way round* the two ends are glued. Walk west out of the garden
	// and the hall has to arrive ahead of you, not behind or beside you.
	//
	// It is also what pins the rails camera in the scene file: those legs are
	// this arithmetic, written out by hand because a script has no `Inverse`.
	const Entity walker = store.CreateInstance(engine::ecs::Classes::Find(Name("Part")), "Walker");

	// **Watched by a camera, because a player is a body and an eye.** The yaw is
	// where a player's view direction actually lives, so a pair that turns a
	// corner has to turn it — a body that comes out walking north under a camera
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
	// to the middle of one steps out an eighth past the other — the thinness is
	// the scene's, and it is what stops "inside the pane" being somewhere a
	// character can stand.
	//
	// **And exactly there, because `scene`'s `LANDING_CLEARANCE` is a floor
	// rather than an offset.** A crosser whose step already ended clear of the
	// plane gets nothing added — adding it unconditionally moved a body a little
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

	// **And the eye turns with it — on the machine the eye is on.** The
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
	// property grid — because an editor half on each is two widget sets — so
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
	// generated row — the `gui`-to-`script` join — and `StarterGui` containment,
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
	// count is asserted so that a panel which silently built nothing — the
	// failure a containment or layout regression produces — cannot pass.
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
	// `Position` would put them at zero — this separates all three.
	CHECK(placed->AbsolutePosition.X > 960.0f);
	CHECK(placed->AbsolutePosition.X < 1920.0f - 300.0f);
}

namespace {

	// Every voxel box the terrain scene built, as a canonical sorted list.
	//
	// Sorted rather than taken in iteration order, because two worlds built the
	// same way are only guaranteed to hold the same *set* of rows — an
	// archetype walk is free to visit them in a different sequence, and a
	// determinism check that compared sequences would fail for a reason that has
	// nothing to do with the generator.
	std::vector<std::array<float, 6>> VoxelBoxes(Store &store) {
		std::vector<std::array<float, 6>> boxes;

		store.Each<const engine::scene::Transform, const engine::scene::Bounds, const Visual>(
			[&](Entity entity,
				const engine::scene::Transform &transform,
				const engine::scene::Bounds &bounds,
				const Visual &visual) {
				if (store.InstanceNameOf(entity) != Name("Voxels") || !visual.Visible) {
					return;
				}

				const engine::core::Vector3 &at = transform.Frame.Position;
				boxes.push_back(
					{at.X, at.Y, at.Z, bounds.HalfExtent.X, bounds.HalfExtent.Y, bounds.HalfExtent.Z}
				);
			}
		);

		std::sort(boxes.begin(), boxes.end());
		return boxes;
	}
}

TEST_CASE("the terrain scene generates a voxel world from noise", "[examples][scene]") {
	const StagedAssets assets;

	Store store("terrain");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Terrain.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// The prefill runs before the first frame, so a world exists the moment the
	// scene is loaded rather than one beat later. Nine chunks of it — the exact
	// count depends on the terrain the camera starts over, so this asserts the
	// order of magnitude and not a number that would have to be edited every
	// time a constant moved.
	const std::vector<std::array<float, 6>> boxes = VoxelBoxes(store);
	CHECK(boxes.size() > 100);
	CHECK(boxes.size() < 20000);

	// **The merge actually merged.** A box wider or deeper than one metre is a
	// run of voxels that became one part, and this is the assertion that
	// separates "the generator emitted something" from "the generator emitted a
	// quarter of a million one-metre cubes". Half-extents, so 0.5 is one block.
	size_t merged = 0;
	for (const std::array<float, 6> &box : boxes) {
		if (box[3] > 0.5f || box[5] > 0.5f) {
			merged++;
		}
	}
	CHECK(merged > boxes.size() / 4);

	// Nothing reaches below bedrock or above the height field's ceiling. A
	// generator that produced a column stretching to the origin is the failure
	// this catches, and it is invisible in a part count.
	for (const std::array<float, 6> &box : boxes) {
		const float bottom = box[1] - box[4];
		const float top = box[1] + box[4];
		CHECK(bottom >= -8.0f);
		CHECK(top <= 200.0f);
	}

	// The camera the scene placed, above the ground rather than inside it.
	REQUIRE(store.Resource<ActiveCamera>() != nullptr);

	const Entity eye = InScene(store, "Surveyor");
	REQUIRE(eye != engine::ecs::NULL_ENTITY);
	CHECK(store.Get<engine::scene::Transform>(eye)->Frame.Position.Y > 34.0f);

	// Measured bounds, not declared. A streamed world reaches as far as what is
	// loaded, which is the camera's neighbourhood rather than the whole map —
	// the 16384-block extent exists as a function and never as geometry.
	REQUIRE(store.Resource<WorldBounds>() != nullptr);
	CHECK(store.Resource<WorldBounds>()->HalfExtent > 100.0f);
}

TEST_CASE("the terrain generator is a pure function of its seed", "[examples][scene]") {
	const StagedAssets assets;

	// **Rule 5, asserted rather than asserted-in-a-comment.** The map is
	// 268 million columns and is never stored, so every block anybody ever sees
	// comes out of `HeightAt` — which means a recording replays if and only if
	// two runs of that function agree. The integer hashing exists for this, and
	// a change that reached for `math.random` or wall time would pass every
	// other check in this file.
	std::vector<std::array<float, 6>> first;
	std::vector<std::array<float, 6>> second;

	for (std::vector<std::array<float, 6>> *into : {&first, &second}) {
		Store store("terrain.determinism");
		Scheduler systems;

		std::string error;
		const bool loaded = LoadScene(store, systems, ExamplePath("Terrain.luau"), error);
		INFO(error);
		REQUIRE(loaded);

		// Ten fixed ticks each, so the camera moves and the streaming runs —
		// comparing only the prefill would pin the generator and leave the part
		// of the file that decides *when* a chunk is built untested.
		for (int tick = 0; tick < 10; tick++) {
			systems.Tick(store, 1.0f / 60.0f);
		}

		*into = VoxelBoxes(store);
	}

	REQUIRE(first.size() == second.size());
	CHECK(first == second);
}

TEST_CASE("the shipped Luau libraries mount and run", "[examples][scene]") {
	const StagedAssets assets;

	Store store("libraries");
	Scheduler systems;

	std::string error;
	const bool loaded = LoadScene(store, systems, ExamplePath("Libraries.luau"), error);
	INFO(error);
	REQUIRE(loaded);

	// **Mounted where a Rojo place would put them**, which is the whole contract
	// the ported libraries were written against.
	const Entity replicated = store.FindFirstRoot("ReplicatedStorage");
	REQUIRE(replicated != engine::ecs::NULL_ENTITY);

	const Entity magic = store.FindFirstChild(replicated, "MagicCore");
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

	const Entity terrain = store.FindFirstChild(replicated, "TerrainCore");
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
