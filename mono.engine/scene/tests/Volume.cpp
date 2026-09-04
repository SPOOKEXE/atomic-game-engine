#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Volume.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace {
	using engine::core::CFrame;
	using engine::core::Vector3;
	using engine::ecs::Store;
	using engine::scene::RegisterSceneClasses;
	using engine::scene::Transform;
	using engine::scene::Volume;
	using engine::scene::VolumeShape;
	using engine::scene::VolumeState;

	TEST_CASE("volumes resolve into bounded presentation values", "[scene][volume]") {
		RegisterSceneClasses();
		Store store("volume.resolve");
		const auto disabled = store.Create();
		store.Set(disabled, Volume{.Enabled = false});
		store.Set(disabled, Transform{});

		const auto source = store.Create();
		store.Set(
			source,
			Volume{
				.Colour = {0.4f, 0.5f, 0.6f},
				.HalfExtent = {4.0f, 5.0f, 6.0f},
				.Density = -1.0f,
				.Extinction = -2.0f,
				.Falloff = 2.0f,
				.NoiseScale = 0.0f,
				.NoiseStrength = 2.0f,
				.Steps = 100,
				.ShadowSteps = 100,
				.Seed = 73,
				.Shape = VolumeShape::Ellipsoid,
			}
		);
		store.Set(source, Transform{.Frame = CFrame{Vector3{3.0f, 2.0f, 1.0f}}});

		std::array<VolumeState, 1> resolved;
		REQUIRE(engine::scene::ResolveVolumes(store, resolved) == 1);
		const VolumeState &volume = resolved.front();
		CHECK(volume.Enabled);
		CHECK(volume.Frame.Position == Vector3{3.0f, 2.0f, 1.0f});
		CHECK(volume.Colour.R == 0.4f);
		CHECK(volume.HalfExtent == Vector3{4.0f, 5.0f, 6.0f});
		CHECK(volume.Density == 0.0f);
		CHECK(volume.Extinction == 0.0f);
		CHECK(volume.Falloff == 1.0f);
		CHECK(volume.NoiseScale == 0.001f);
		CHECK(volume.NoiseStrength == 1.0f);
		CHECK(volume.Steps == 64);
		CHECK(volume.ShadowSteps == 32);
		CHECK(volume.Seed == 73);
		CHECK(volume.Shape == VolumeShape::Ellipsoid);
	}
}
