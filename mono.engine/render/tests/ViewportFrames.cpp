#include "RenderFixture.hpp"
#include "ViewportFrameScene.hpp"

#include <engine/assets/Texture.hpp>
#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/DrawList.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/render/ViewportFrames.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::render::CollectViewportInstances;
using engine::scene::DrawInstance;

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
	REQUIRE(frames.Render(fixture.Render, store, commands, 1, firstOwner) == 1);
	const auto capture = [&](Name owner) {
		REQUIRE(frames.Render(fixture.Render, store, commands, 1, owner) == 1);
		CHECK(frames.Resolve(viewport).Texture != nullptr);
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
