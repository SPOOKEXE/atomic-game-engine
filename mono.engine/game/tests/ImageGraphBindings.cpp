#include <engine/game/Game.hpp>
#include <engine/scene/ImageGraphBinding.hpp>
#include <engine/scene/Part.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_SUITE_ID("engine.game.imagegraphbindings")
TEST_DEPENDS("engine.scene.imagegraphbinding")

TEST_CASE("authored image graph bindings survive standard instance documents", "[game][imagegraphbinding]") {
	using engine::core::Name;
	engine::ecs::Store source("composer-authored");
	const auto sink = source.CreateInstance(engine::scene::PartClass(), "Consumer");
	engine::scene::ImageGraphBinding binding;
	binding.Graph = Name("Composer-Material-Maps");
	binding.Output = Name("colour");
	binding.Texture = Name("composer-colour");
	binding.Seed = 0xFEDCBA9876543210ull;
	binding.FixedTick = 31;
	binding.TickPolicy = engine::scene::ImageGraphTickPolicy::World;
	binding.ColorSpace = engine::scene::ImageGraphColorSpace::Linear;
	REQUIRE(engine::scene::SetImageGraphBinding(source, sink, binding));
	const std::string document = engine::game::WriteInstanceDocument(source, sink);
	CHECK(document.find("Composer-Material-Maps") != std::string::npos);
	CHECK(document.find("ImageGraphTickPolicy") != std::string::npos);
	engine::ecs::Store restored("composer-restored");
	std::string error;
	const auto loaded =
		engine::game::ReadInstanceDocument(restored, document, engine::ecs::NULL_ENTITY, error);
	INFO(error);
	REQUIRE(loaded != engine::ecs::NULL_ENTITY);
	const auto *actual = restored.Get<engine::scene::ImageGraphBinding>(loaded);
	REQUIRE(actual != nullptr);
	CHECK(actual->Graph == binding.Graph);
	CHECK(actual->Output == binding.Output);
	CHECK(actual->Texture == binding.Texture);
	CHECK(actual->Seed == binding.Seed);
	CHECK(actual->FixedTick == binding.FixedTick);
	CHECK(actual->TickPolicy == binding.TickPolicy);
	CHECK(actual->ColorSpace == binding.ColorSpace);
}
