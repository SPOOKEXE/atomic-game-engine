#include "ViewportFrameScene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <vector>

using engine::core::Name;
using engine::ecs::Classes;
using engine::ecs::Entity;
using engine::ecs::Store;
using engine::render::CollectViewportInstances;
using engine::scene::DrawInstance;

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
