// Physical particle rows skip particles too small to draw from the camera.

#include <engine/core/types/CFrame.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/cameras/ActiveCamera.hpp>
#include <engine/scene/environment/PhysicalParticleField.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.scene.physicalparticlerows")

namespace {
	using namespace engine;

	// One particle straight ahead of the origin, `metres` away.
	scene::PhysicalParticle At(float metres, uint64_t id) {
		scene::PhysicalParticle particle;
		particle.Position = {0, 0, static_cast<int64_t>(-metres * 65536.0f)};
		particle.Previous = particle.Position;
		particle.Id = id;
		return particle;
	}

	std::vector<scene::DrawInstance> Rows(ecs::Store &store) {
		std::vector<scene::DrawInstance> rows;
		scene::AppendPhysicalParticleRows(store, rows);
		return rows;
	}
}

TEST_CASE("particles under half a pixel across are not drawn", "[scene][particles]") {
	ecs::Store store("particle-rows");
	const ecs::Entity eye = store.Create();
	store.Set(eye, scene::Camera{});
	store.Set(eye, scene::Transform{core::CFrame()});
	store.SetResource(scene::ActiveCamera{eye});

	scene::PhysicalParticleRecord record;
	record.Settings.Radius = 0.09f;
	record.Alpha = 1.0f;
	// A 0.18 m hailstone at the default 70 degree field of view on a
	// 1000-pixel-tall view is half a pixel across at about 257 m.
	record.Particles = {At(10.0f, 1), At(400.0f, 2)};
	store.SetResource(scene::PhysicalParticleWorld{{record}});

	// Without a viewport size there is no pixel scale, so nothing is skipped.
	CHECK(Rows(store).size() == 2);

	REQUIRE(scene::SetViewportSize(store, 1000, 1000));
	CHECK(store.Resource<scene::ActiveCamera>()->ViewportHeight == 1000);
	const auto rows = Rows(store);
	REQUIRE(rows.size() == 1);
	CHECK(rows.front().Variant == 2);
}
