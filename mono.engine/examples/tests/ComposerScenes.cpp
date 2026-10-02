#include <engine/core/Paths.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/effects/Particles.hpp>
#include <engine/effects/Ribbon.hpp>
#include <engine/examples/DemosLoader.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

TEST_SUITE_ID("engine.examples.composer-scenes")
TEST_DEPENDS("engine.scene.imagegraphbinding")
TEST_DEPENDS("engine.examples.demos-loader")

TEST_CASE(
	"authored Composer scenes build valid live consumers from embedded scripts", "[examples][composer]"
) {
	const std::string scene = GENERATE(
		"Composer-Material-Maps",
		"Composer-Gui-And-Shader",
		"Composer-Skybox",
		"Composer-Particle-Flipbook",
		"Composer-3D-Reference",
		"Composer-Reference-Studies",
		"Composer-Feedback-And-Fluid"
	);
	const std::filesystem::path assets = engine::core::Paths::Base().parent_path() / "assets";
	const engine::examples::DemosLoader demos(assets / "examples");
	const auto path = demos.Resolve(engine::examples::DemoKind::World, scene + ".aworld");
	REQUIRE_FALSE(path.empty());
	std::ifstream input(path);
	const std::string document(std::istreambuf_iterator<char>(input), {});
	const auto start = document.find("<![CDATA[");
	const auto end = document.find("]]>", start);
	REQUIRE(start != std::string::npos);
	REQUIRE(end != std::string::npos);
	const auto scriptPath = std::filesystem::temp_directory_path() / (scene + ".luau");
	struct RemoveScript {
		std::filesystem::path Path;
		~RemoveScript() {
			std::error_code ignored;
			std::filesystem::remove(Path, ignored);
		}
	} cleanup{scriptPath};
	{
		std::ofstream script(scriptPath);
		script << document.substr(start + 9, end - start - 9);
	}
	engine::ecs::Store store(scene);
	engine::ecs::Scheduler scheduler;
	std::string error;
	REQUIRE(engine::examples::LoadScene(store, scheduler, scriptPath, error));
	INFO(error);
	std::set<std::string> outputs;
	std::set<std::string> graphs;
	std::set<std::pair<std::string, std::string>> selections;
	size_t bindingCount = 0;
	store.Each<const engine::scene::ImageGraphBinding>([&](engine::ecs::Entity, const auto &binding) {
		++bindingCount;
		selections.emplace(binding.Graph.Text(), binding.Output.Text());
		CHECK(engine::scene::IsValidImageGraphBinding(binding));
		if (scene == "Composer-Reference-Studies")
			CHECK(
				(binding.Graph.Text().starts_with("Composer-Study-") ||
				 binding.Graph.Text().starts_with("Composer-Source-"))
			);
		else if (scene == "Composer-Feedback-And-Fluid")
			CHECK((binding.Graph.Text() == scene || binding.Graph.Text() == "Composer-Fluid-Particles"));
		else
			CHECK(binding.Graph.Text() == scene);
		graphs.emplace(binding.Graph.Text());
		CHECK(
			binding.TickPolicy == (scene == "Composer-Feedback-And-Fluid"
									   ? engine::scene::ImageGraphTickPolicy::Fixed
									   : engine::scene::ImageGraphTickPolicy::World)
		);
		CHECK(
			std::filesystem::is_regular_file(
				assets / "imagegraphs" / (std::string(binding.Graph.Text()) + ".graph")
			)
		);
		outputs.emplace(binding.Output.Text());
	});

	if (scene == "Composer-Particle-Flipbook") {
		engine::ecs::Entity spark = engine::ecs::NULL_ENTITY, fire = engine::ecs::NULL_ENTITY,
							beam = engine::ecs::NULL_ENTITY, trail = engine::ecs::NULL_ENTITY;
		store.Each<const engine::effects::ParticleEmitter>([&](engine::ecs::Entity entity, const auto &) {
			if (store.InstanceNameOf(entity) == engine::core::Name("Fire")) fire = entity;
			if (store.InstanceNameOf(entity) == engine::core::Name("Sparks")) spark = entity;
		});
		store.Each<const engine::effects::Beam>([&](engine::ecs::Entity entity, const auto &ribbon) {
			beam = entity;
			CHECK(ribbon.Texture == engine::core::Name("composer-fire"));
		});
		store.Each<const engine::effects::Trail>([&](engine::ecs::Entity entity, const auto &ribbon) {
			trail = entity;
			CHECK(ribbon.Texture == engine::core::Name("composer-spark"));
		});
		REQUIRE(fire != engine::ecs::NULL_ENTITY);
		REQUIRE(spark != engine::ecs::NULL_ENTITY);
		REQUIRE(beam != engine::ecs::NULL_ENTITY);
		REQUIRE(trail != engine::ecs::NULL_ENTITY);
		scheduler.Tick(store, 11.f);
		CHECK(store.Get<engine::effects::ParticleEmitter>(fire) != nullptr);
		CHECK(store.Get<engine::effects::ParticleEmitter>(spark) == nullptr);
		CHECK(store.Get<engine::effects::Beam>(beam) == nullptr);
		CHECK(store.Get<engine::effects::Trail>(trail) == nullptr);
		size_t survivors = 0;
		store.Each<const engine::scene::ImageGraphBinding>([&](engine::ecs::Entity, const auto &binding) {
			++survivors;
			CHECK(binding.Output == engine::core::Name("fire"));
		});
		CHECK(survivors == 1);
	}
	if (scene == "Composer-Skybox")
		CHECK(outputs == std::set<std::string>{"front", "back", "left", "right", "up", "down"});
	else if (scene == "Composer-Material-Maps")
		CHECK(outputs == std::set<std::string>{"colour", "normal", "pbr"});
	else if (scene == "Composer-Gui-And-Shader")
		CHECK(outputs == std::set<std::string>{"mask", "height", "colour"});
	else if (scene == "Composer-Feedback-And-Fluid")
		CHECK(outputs == std::set<std::string>{"accumulated", "fluid"});
	else if (scene == "Composer-Reference-Studies") {
		CHECK(
			outputs == std::set<std::string>{
						   "study",
						   "gSKBQW2707447cZLPHO4qsosZuR6bD0L",
						   "gSKBO82563685pOhaulkBGjkN0ZahGhW",
						   "gSKAiP180467pJ0YpXbwrplRMgj671sM",
						   "gSKBQu2731320Bc3gLD9ntjq2djDzQT3",
						   "gSKA1Q1237222AW3R5nxuMfz0TUM1wYS",
					   }
		);
		CHECK(graphs.size() == 10);
		CHECK(bindingCount == 10);
		CHECK(selections.contains({"Composer-Source-Black-Hole", "gSKBQW2707447cZLPHO4qsosZuR6bD0L"}));
		CHECK(selections.contains({"Composer-Study-Black-Hole", "study"}));
		CHECK(selections.contains({"Composer-Source-Fire-Tornado", "gSKBO82563685pOhaulkBGjkN0ZahGhW"}));
		CHECK(selections.contains({"Composer-Study-Fire-Tornado", "study"}));
		CHECK(selections.contains(
			{"Composer-Source-Glass-Block-Refraction", "gSKAiP180467pJ0YpXbwrplRMgj671sM"}
		));
		CHECK(selections.contains({"Composer-Study-Glass-Block-Refraction", "study"}));
		CHECK(selections.contains({"Composer-Source-Ornate-Trim", "gSKBQu2731320Bc3gLD9ntjq2djDzQT3"}));
		CHECK(selections.contains({"Composer-Study-Ornate-Trim", "study"}));
		CHECK(selections.contains({"Composer-Source-Spark-Bolt", "gSKA1Q1237222AW3R5nxuMfz0TUM1wYS"}));
		CHECK(selections.contains({"Composer-Study-Spark-Bolt", "study"}));

	} else if (scene == "Composer-3D-Reference")
		CHECK(
			outputs == std::set<std::string>{
						   "rendered",
						   "depth",
						   "normal",
						   "raymarch",
						   "transform",
						   "diffuse",
						   "view_normal",
						   "shadow",
						   "ambient_occlusion"
					   }
		);
	else
		CHECK(outputs == std::set<std::string>{"fire", "spark"});
}
