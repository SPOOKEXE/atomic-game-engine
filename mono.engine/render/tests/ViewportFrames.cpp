#include "RenderFixture.hpp"
#include "ViewportFrameScene.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/ViewportFramePolicy.hpp>
#include <engine/render/ViewportFrames.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <vector>

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::render::CollectViewportInstances;
using engine::scene::DrawInstance;

TEST_CASE("ViewportFrame cache modes refresh only on their declared schedule", "[render][viewportframe]") {
	using engine::gui::ViewportUpdateMode;
	using engine::render::ViewportFrameCache;
	using engine::render::ViewportFrameTargetOwner;

	ViewportFrameCache cache;
	CHECK(cache.NeedsRender(ViewportUpdateMode::OnChange, 1, 0, 11, 1));
	cache.Commit(11, 1, 0);
	CHECK_FALSE(cache.NeedsRender(ViewportUpdateMode::OnChange, 1, 0, 11, 2));
	CHECK(cache.NeedsRender(ViewportUpdateMode::OnChange, 1, 0, 12, 2));
	CHECK(cache.NeedsRender(ViewportUpdateMode::EveryFrame, 1, 0, 11, 2));
	CHECK_FALSE(cache.NeedsRender(ViewportUpdateMode::FixedRate, 3, 0, 11, 3));
	CHECK(cache.NeedsRender(ViewportUpdateMode::FixedRate, 3, 0, 11, 4));
	CHECK_FALSE(cache.NeedsRender(ViewportUpdateMode::Manual, 1, 0, 11, 100));
	CHECK(cache.NeedsRender(ViewportUpdateMode::Manual, 1, 1, 11, 100));

	ViewportFrameTargetOwner target;
	target.Commit(10, 1);
	CHECK(target.Owns(10));
	target.Commit(20, 2);
	CHECK_FALSE(target.Owns(10));
	CHECK(target.Owns(20));
	CHECK_FALSE(target.Owns(0));
	CHECK((!target.Owns(10) || cache.NeedsRender(ViewportUpdateMode::OnChange, 1, 0, 11, 2)));
}

TEST_CASE(
	"viewport frames resolve textures in their containing world owner", "[render][gpu][viewport-owner][.]"
) {
	using namespace engine;
	scene::RegisterSceneClasses();
	gui::RegisterGuiClasses();
	render::test::FixtureDevice fixture;
	fixture.Initialise();
	ecs::Store store("viewport.owner");
	const auto viewport = store.CreateInstance(gui::GuiClass("ViewportFrame"), "Preview");
	const auto camera = store.CreateInstance(Classes::Find(Name("Camera")), "Camera");
	gui::Viewport settings;
	settings.CurrentCamera = camera;
	store.Set(viewport, settings);
	scene::PartDesc part;
	part.Frame.Position = {0, 0, -4};
	part.Size = {4, 4, .1f};
	part.Simulated = false;
	const auto wall = scene::MakePart(store, part);
	REQUIRE(store.SetParent(wall, viewport));
	const Name texture("viewport:same-name"), firstOwner("viewport:first"), secondOwner("viewport:second");
	store.GetMutable<scene::SurfaceAppearance>(wall)->ColourMap = texture;
	assets::TextureData image;
	image.Width = image.Height = 1;
	image.Format = assets::TextureFormat::RGBA8;
	image.Pixels = {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
	REQUIRE(fixture.Render.AddTexture(texture, image, firstOwner));
	const Name redReference("viewport:red-reference"), blueReference("viewport:blue-reference");
	REQUIRE(fixture.Render.AddTexture(redReference, image));
	image.Pixels = {std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255}};
	REQUIRE(fixture.Render.AddTexture(texture, image, secondOwner));
	REQUIRE(fixture.Render.AddTexture(blueReference, image));
	gui::DrawList commands;
	gui::DrawCommand command;
	command.Kind = gui::DrawKind::Viewport;
	command.Source = viewport;
	command.Bounds = {{0, 0}, {64, 64}};
	commands.Commands.push_back(command);
	render::ViewportFrames frames;
	const auto capture = [&](Name owner) {
		void *const previous = frames.Resolve(viewport).Texture;
		render::InterfaceImage frameImage;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline) {
			if (frames.Render(fixture.Render, store, commands, 1, owner) != 1) {
				SDL_Delay(1);
				continue;
			}
			frameImage = frames.Resolve(viewport);
			if (frameImage.Texture != nullptr && frameImage.Texture != previous) break;
			SDL_Delay(1);
		}
		INFO("viewport frame did not complete before the deadline");
		REQUIRE(frameImage.Texture != nullptr);
		REQUIRE(frameImage.Texture != previous);
		return render::test::CaptureResource(
			fixture.Render, Name("albedo"), 1, 64, 64, render::test::ImageFormat::Rgba8Unorm
		);
	};
	const auto red = capture(firstOwner);
	const auto blue = capture(secondOwner);
	const size_t center = 32 * red.RowStrideBytes + 32 * 4;
	CHECK(std::to_integer<int>(red.Bytes[center]) > 200);
	CHECK(std::to_integer<int>(blue.Bytes[center + 2]) > 200);
	CHECK(red.Bytes != blue.Bytes);
	store.GetMutable<scene::SurfaceAppearance>(wall)->ColourMap = redReference;
	CHECK(capture({}).Bytes == red.Bytes);
	store.GetMutable<scene::SurfaceAppearance>(wall)->ColourMap = blueReference;
	CHECK(capture({}).Bytes == blue.Bytes);
}

namespace {
	struct ViewportWorld {
		Store Data{"viewport_frames"};
		Entity Viewport;
		Entity World;

		ViewportWorld() {
			engine::scene::EnsureClassTree();
			engine::gui::RegisterGuiClasses();

			Viewport = Data.CreateInstance(engine::gui::GuiClass("ViewportFrame"), "Preview");
			World = Data.CreateInstance(Classes::Find(Name("WorldModel")), "World");
			REQUIRE(Data.SetParent(World, Viewport));
		}

		Entity Part(std::string_view name, Entity parent) {
			const Entity part = Data.CreateInstance(engine::scene::PartClass(), name);
			REQUIRE(Data.SetParent(part, parent));
			return part;
		}
	};

	bool ContainsSource(const std::vector<DrawInstance> &instances, Entity entity) {
		return std::any_of(instances.begin(), instances.end(), [&](const DrawInstance &instance) {
			return instance.Source == entity.Id;
		});
	}
}

TEST_CASE("a ViewportFrame draws only its own WorldModel descendants", "[render][viewportframe]") {
	ViewportWorld world;

	const Entity direct = world.Part("Direct", world.World);
	const Entity folder = world.Data.CreateInstance(Classes::Find(Name("Instance")), "Group");
	REQUIRE(world.Data.SetParent(folder, world.World));
	const Entity nested = world.Part("Nested", folder);

	const Entity otherWorld = world.Data.CreateInstance(Classes::Find(Name("WorldModel")), "OtherWorld");
	const Entity unrelated = world.Part("Unrelated", otherWorld);

	std::vector<DrawInstance> instances;
	CollectViewportInstances(world.Data, world.Viewport, instances);

	REQUIRE(instances.size() == 2);
	CHECK(ContainsSource(instances, direct));
	CHECK(ContainsSource(instances, nested));
	CHECK_FALSE(ContainsSource(instances, unrelated));
}

TEST_CASE("non-drawable ViewportFrame descendants stay out of the scene", "[render][viewportframe]") {
	ViewportWorld world;

	const Entity folder = world.Data.CreateInstance(Classes::Find(Name("Instance")), "Group");
	REQUIRE(world.Data.SetParent(folder, world.World));

	std::vector<DrawInstance> instances;
	CollectViewportInstances(world.Data, world.Viewport, instances);

	CHECK(instances.empty());
}

TEST_CASE("ViewportFrame collection does not merge a nested viewport world", "[render][viewportframe]") {
	using namespace engine;
	ViewportWorld world;
	const Entity nested = world.Data.CreateInstance(gui::GuiClass("ViewportFrame"), "Nested");
	REQUIRE(world.Data.SetParent(nested, world.World));
	const Entity nestedWorld = world.Data.CreateInstance(Classes::Find(Name("WorldModel")), "NestedWorld");
	REQUIRE(world.Data.SetParent(nestedWorld, nested));
	const Entity nestedPart = world.Part("NestedPart", nestedWorld);

	std::vector<DrawInstance> instances;
	CollectViewportInstances(world.Data, world.Viewport, instances);

	CHECK_FALSE(ContainsSource(instances, nestedPart));
}

TEST_CASE("ViewportFrame ignores local transparency overrides", "[render][viewportframe]") {
	using namespace engine;
	ViewportWorld world;
	const Entity part = world.Part("Opaque", world.World);
	world.Data.Set(part, scene::LocalTransparency{1.0f});

	std::vector<DrawInstance> instances;
	CollectViewportInstances(world.Data, world.Viewport, instances);
	REQUIRE(instances.size() == 1);
	CHECK(instances[0].Transparency == 0.0f);
}

TEST_CASE("ViewportFrame copies optional LOD and effect state", "[render][viewportframe]") {
	using namespace engine;
	ViewportWorld world;
	const Entity part = world.Part("Detailed", world.World);

	scene::LODAuto automatic;
	automatic.Meshes[0] = Name("viewport.auto-half");
	automatic.Meshes[1] = Name("viewport.auto-quarter");
	automatic.Levels = 3;
	world.Data.Set(part, automatic);
	scene::LODCustom custom;
	custom.Meshes[0] = Name("viewport.custom-half");
	custom.Levels = 3;
	world.Data.Set(part, custom);
	scene::LODSettings settings;
	settings.MinimumDistances[0] = 12.0f;
	world.Data.Set(part, settings);
	scene::RenderEffects effects;
	effects.Attachments[0].Node = Name("viewport-outline");
	effects.Attachments[0].Enabled = true;
	effects.Count = 1;
	world.Data.Set(part, effects);

	std::vector<DrawInstance> instances;
	CollectViewportInstances(world.Data, world.Viewport, instances);
	REQUIRE(instances.size() == 1);
	CHECK(instances[0].LodMeshes[0] == Name("viewport.custom-half"));
	CHECK(instances[0].LodMeshes[1] == Name("viewport.auto-quarter"));
	CHECK(instances[0].LodMinimumDistances[0] == 12.0f);
	CHECK(instances[0].Effects.Count == 1);
	CHECK(instances[0].Effects.Attachments[0].Node == Name("viewport-outline"));
}
