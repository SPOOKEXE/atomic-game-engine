// Light carried through portal seams, measured on the scenes authored for it.
//
// **The transport under test is `client::CollectLights`' seam-copy pass**, and
// the fixtures are the two shipped scenes rather than hand-built stores, so
// what the GPU capture script points a camera at and what this suite asserts
// are one authored arrangement. What a headless test can decide is the light
// list the renderer is handed: that a lamp standing in a pane gains a copy on
// the far mouth, that two lamps either side each cross into the other room,
// and that `Portal.Enabled = false` withdraws the copies. What only a capture
// can decide - the lit pixels - is `scripts/demos/capture-portal-lighting.sh`.

#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/Components.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <client/Scene.hpp>
#include <cmath>
#include <limits>
#include <vector>

TEST_SUITE_ID("client.scene.portallighting")
TEST_DEPENDS("engine.scene.surfacecameras")
TEST_DEPENDS("engine.examples.scene")

using Catch::Approx;
using engine::core::Name;
using engine::core::Vector3;
using engine::ecs::Entity;
using engine::render::SceneLight;

namespace {
	// The two scenes' shared layout, restated so a drifted constant fails here
	// rather than passing against itself.
	constexpr float GAP = 120.0f;
	constexpr float FAR_PANE_X = GAP - 10.0f;

	// A world built from one of the portal lighting scenes, ready to collect.
	struct LitScene {
		engine::ecs::Store World{"portal-lighting"};
		engine::ecs::Scheduler Systems;

		explicit LitScene(const char *scene) {
			engine::parallel::Jobs::Start(2);

			// The staged assets root, not the test binary's own directory -
			// `SceneTick.cpp`'s reason.
			engine::core::Paths::SetAssetsOverride(engine::core::Paths::Base().parent_path() / "assets");

			const bool built =
				client::BuildScriptedWorld(World, Systems, engine::examples::ExamplePath(scene), 64);
			REQUIRE(built);

			// One tick so every load-time write has settled the way a client's
			// first frame would have it.
			Systems.Tick(World, 1.0f / 60.0f);
		}

		~LitScene() {
			engine::parallel::Jobs::Stop();
		}

		std::vector<SceneLight> Collect() {
			std::vector<SceneLight> lights;
			client::CollectLights(World, Vector3{}, lights);
			return lights;
		}

		// Every portal mouth in the scene, for the Enabled cases.
		std::vector<Entity> Portals() {
			std::vector<Entity> mouths;
			World.Each<const engine::scene::Portal>([&](Entity entity, const engine::scene::Portal &) {
				mouths.push_back(entity);
			});
			return mouths;
		}
	};

	// The light nearest `at`, so an assertion names a place rather than an
	// index - the collect order is authored-then-copies today, and nothing
	// about the transport promises it.
	const SceneLight &NearestTo(const std::vector<SceneLight> &lights, const Vector3 &at) {
		REQUIRE(!lights.empty());
		const SceneLight *found = &lights.front();
		float nearest = std::numeric_limits<float>::max();
		for (const SceneLight &light : lights) {
			const Vector3 offset = light.Position - at;
			const float distance = offset.Dot(offset);
			if (distance < nearest) {
				nearest = distance;
				found = &light;
			}
		}
		return *found;
	}
}

TEST_CASE("a lamp in a pane is copied onto the far mouth", "[client][portal][lighting]") {
	LitScene scene("PortalLightOut.luau");

	const auto lights = scene.Collect();

	// One authored lamp, one copy - and only one: the lamp is 110 studs from
	// the far pane's rectangle, so the return seam makes nothing of it.
	REQUIRE(lights.size() == 2);

	const SceneLight authored = NearestTo(lights, Vector3{0.0f, 5.0f, 0.0f});
	CHECK(authored.Position.X == Approx(0.0f).margin(0.01f));
	CHECK(authored.Position.Y == Approx(5.0f).margin(0.01f));
	CHECK(authored.Position.Z == Approx(0.0f).margin(0.01f));

	// The copy lands on the far pane's centre, because the map carries the near
	// plane onto the far one and the lamp stands in the near plane. The margin
	// is the pane's own thickness: a seam sits on the pane's *face*, so a lamp
	// at the slab's centre is a face's depth behind it and its copy the same
	// depth beyond the far face.
	const SceneLight copy = NearestTo(lights, Vector3{FAR_PANE_X, 5.0f, 0.0f});
	CHECK(copy.Position.X == Approx(FAR_PANE_X).margin(0.25f));
	CHECK(copy.Position.Y == Approx(5.0f).margin(0.05f));
	CHECK(copy.Position.Z == Approx(0.0f).margin(0.05f));

	// The lamp itself, not a variation of it: same colour, and a range the
	// unscaled seam maps through unchanged.
	CHECK(copy.Colour.R == Approx(authored.Colour.R));
	CHECK(copy.Colour.G == Approx(authored.Colour.G));
	CHECK(copy.Colour.B == Approx(authored.Colour.B));
	CHECK(copy.Range == Approx(authored.Range));
	CHECK(copy.Range == Approx(25.0f));
}

TEST_CASE("two lamps either side of a hole each cross into the other room", "[client][portal][lighting]") {
	LitScene scene("PortalLightMix.luau");

	const auto lights = scene.Collect();

	// Red and green authored, and one copy each through its own seam.
	REQUIRE(lights.size() == 4);

	// The authored pair, where the scene puts them: red 4 in front of the near
	// pane, green 4 in front of the far one.
	const SceneLight red = NearestTo(lights, Vector3{0.0f, 6.0f, -4.0f});
	CHECK(red.Colour.R > red.Colour.G * 2.0f);

	const SceneLight green = NearestTo(lights, Vector3{GAP - 6.0f, 6.0f, 0.0f});
	CHECK(green.Colour.G > green.Colour.R * 2.0f);

	// The transported pair: each lands the same distance *behind* the far
	// mouth, shining into the room the hole shows. Green in the near room is
	// the light that can only have crossed - the scene authors no green lamp
	// on that side - and red in the far room is its mirror.
	// Margins are the pane's thickness: seams sit on the panes' faces, so each
	// copy is a face's depth off the slab-centre arithmetic these constants use.
	const SceneLight greenCopy = NearestTo(lights, Vector3{0.0f, 6.0f, 4.0f});
	CHECK(greenCopy.Colour.G > greenCopy.Colour.R * 2.0f);
	CHECK(greenCopy.Position.Z == Approx(4.0f).margin(0.25f));
	CHECK(greenCopy.Position.X == Approx(0.0f).margin(0.05f));

	const SceneLight redCopy = NearestTo(lights, Vector3{FAR_PANE_X - 4.0f, 6.0f, 0.0f});
	CHECK(redCopy.Colour.R > redCopy.Colour.G * 2.0f);
	CHECK(redCopy.Position.X == Approx(FAR_PANE_X - 4.0f).margin(0.25f));
	CHECK(redCopy.Position.Z == Approx(0.0f).margin(0.05f));

	// Both rooms hold both colours: the mix the capture script measures on the
	// floor exists in the list the renderer is handed.
	const auto inNearRoom = [](const SceneLight &light) { return std::abs(light.Position.X) < 40.0f; };
	CHECK(inNearRoom(red));
	CHECK(inNearRoom(greenCopy));
	CHECK(!inNearRoom(green));
	CHECK(!inNearRoom(redCopy));
}

TEST_CASE("a disabled portal withdraws its light-field capture views", "[client][portal][lighting]") {
	LitScene scene("PortalLightMix.luau");

	// The renderer's seam light-field capture renders one probe per
	// `PortalView` handed to it, so the view list is the capture path's whole
	// input - two mouths while the pair is enabled.
	std::vector<engine::render::PortalView> views;
	REQUIRE(client::CollectPortalViews(scene.World, views) == 2);

	// Turning the mouths off withdraws the seams, so the renderer is handed no
	// views, captures no light fields, and projects nothing - `Portal.Enabled`
	// is the switch the roadmap's capture-path item names.
	for (const Entity mouth : scene.Portals()) {
		scene.World.GetMutable<engine::scene::Portal>(mouth)->Enabled = false;
	}
	CHECK(client::CollectPortalViews(scene.World, views) == 0);
}

TEST_CASE("a disabled portal carries no light", "[client][portal][lighting]") {
	LitScene scene("PortalLightMix.luau");
	REQUIRE(scene.Collect().size() == 4);

	// Turning the mouths off withdraws the seams, so only the authored lamps
	// remain - the switch the roadmap's capture-path item relies on.
	for (const Entity mouth : scene.Portals()) {
		scene.World.GetMutable<engine::scene::Portal>(mouth)->Enabled = false;
	}

	const auto lights = scene.Collect();
	REQUIRE(lights.size() == 2);
	for (const SceneLight &light : lights) {
		// What is left is authored: one in each room, none on a pane's far side.
		const bool nearRoom = std::abs(light.Position.X) < 40.0f;
		const bool farRoom = std::abs(light.Position.X - GAP) < 40.0f;
		CHECK((nearRoom || farRoom));
	}
}
