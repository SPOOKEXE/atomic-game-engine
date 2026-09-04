#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/ShaderLens.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <vector>

namespace {
	using engine::core::CFrame;
	using engine::core::Name;
	using engine::core::Vector3;
	using engine::ecs::Store;
	using engine::scene::RegisterSceneClasses;
	using engine::scene::ShaderLens;
	using engine::scene::ShaderLensState;
	using engine::scene::Transform;

	TEST_CASE("shader lenses resolve valid entries in priority order", "[scene][shader-lens]") {
		RegisterSceneClasses();
		Store store("shader-lens.resolve");
		const auto disabled = store.Create();
		store.Set(disabled, ShaderLens{.Shader = Name("disabled"), .Enabled = false});
		store.Set(disabled, Transform{});

		const auto later = store.Create();
		store.Set(
			later,
			ShaderLens{
				.Shader = Name("gravitational-lens"),
				.Radius = 8.0f,
				.InnerRadius = 20.0f,
				.Falloff = 2.0f,
				.Strength = -1.0f,
				.Spin = 0.4f,
				.Priority = 4,
			}
		);
		store.Set(later, Transform{.Frame = CFrame{Vector3{4.0f, 3.0f, 2.0f}}});

		const auto earlier = store.Create();
		store.Set(earlier, ShaderLens{.Shader = Name("heat-haze"), .Radius = 5.0f, .Priority = -2});
		store.Set(earlier, Transform{});

		std::array<ShaderLensState, 2> resolved;
		REQUIRE(engine::scene::ResolveShaderLenses(store, resolved) == 2);
		CHECK(resolved[0].Shader == Name("heat-haze"));
		CHECK(resolved[1].Shader == Name("gravitational-lens"));
		CHECK(resolved[1].Frame.Position == Vector3{4.0f, 3.0f, 2.0f});
		CHECK(resolved[1].InnerRadius == 8.0f);
		CHECK(resolved[1].Falloff == 1.0f);
		CHECK(resolved[1].Strength == 0.0f);

		std::vector<Name> demanded;
		REQUIRE(engine::scene::DemandedLensShaders(store, demanded) == 2);
		CHECK(demanded[0] == Name("gravitational-lens"));
		CHECK(demanded[1] == Name("heat-haze"));
	}
}

TEST_CASE("shader lens selection retains the highest priorities within its budget", "[scene][shader-lens]") {
	RegisterSceneClasses();
	Store store("shader-lens.budget");
	for (int32_t priority = 0; priority <= 16; priority++) {
		const auto entity = store.Create();
		store.Set(entity, ShaderLens{.Shader = Name("gravitational-lens"), .Priority = priority});
		store.Set(entity, Transform{});
	}

	std::array<ShaderLensState, engine::scene::MAX_SCENE_SHADER_LENSES> resolved;
	REQUIRE(engine::scene::ResolveShaderLenses(store, resolved) == resolved.size());
	CHECK(resolved.front().Priority == 1);
	CHECK(resolved.back().Priority == 16);
}

TEST_CASE("shader lens priority remains ordered when shader names interleave", "[scene][shader-lens]") {
	RegisterSceneClasses();
	Store store("shader-lens.interleaved");
	for (const ShaderLens &lens : std::array{
			 ShaderLens{.Shader = Name("warp"), .Priority = 2},
			 ShaderLens{.Shader = Name("heat"), .Priority = 3},
			 ShaderLens{.Shader = Name("warp"), .Priority = 4},
		 }) {
		const auto entity = store.Create();
		store.Set(entity, lens);
		store.Set(entity, Transform{});
	}

	std::array<ShaderLensState, 3> resolved;
	REQUIRE(engine::scene::ResolveShaderLenses(store, resolved) == resolved.size());
	CHECK(resolved[0].Shader == Name("warp"));
	CHECK(resolved[1].Shader == Name("heat"));
	CHECK(resolved[2].Shader == Name("warp"));
}
