#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/PhysicalParticles.hpp>
#include <engine/game/WeatherGround.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/PhysicalParticleField.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Suite.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("engine.game.physicalparticles")
TEST_DEPENDS("engine.weather.runtime")

using namespace engine;
using Catch::Approx;

namespace {
	void Register() {
		scene::RegisterSceneClasses();
		weather::RegisterWeatherRuntimeComponents();
		physics::RegisterPhysicsComponents();
	}

	// An anchored slab whose top face is at y = 1.
	void Ground(ecs::Store &store) {
		const auto ground = store.Create();
		store.Set(ground, scene::Transform{core::CFrame({0, 0, 0})});
		scene::Collider collider;
		collider.Extent = {200, 1, 200};
		store.Set(ground, collider);
	}

	ecs::Entity Field(ecs::Store &store, scene::ParticleResponse response) {
		const auto field = store.Create("Hail");
		store.Set(field, scene::Transform{core::CFrame({0, 20, 0})});
		scene::PhysicalParticleField settings;
		settings.HalfExtent = {10, 10, 10};
		settings.Rate = 60;
		settings.Lifetime = 4;
		settings.Restitution = 0.2f;
		settings.MaxEventsPerStep = 256;
		settings.Response = response;
		store.Set(field, settings);
		return field;
	}

	const std::vector<scene::PhysicalParticle> &Particles(const ecs::Store &store) {
		static const std::vector<scene::PhysicalParticle> none;
		const auto *world = store.Resource<scene::PhysicalParticleWorld>();
		return world && !world->Records.empty() ? world->Records.front().Particles : none;
	}

	bool Same(const std::vector<scene::PhysicalParticle> &a, const std::vector<scene::PhysicalParticle> &b) {
		if (a.size() != b.size()) return false;
		for (size_t index = 0; index < a.size(); ++index) {
			if (a[index].Id != b[index].Id || a[index].Position != b[index].Position ||
				a[index].Velocity != b[index].Velocity || a[index].Resting != b[index].Resting)
				return false;
		}
		return true;
	}

	struct World {
		ecs::Store Store;
		ecs::Scheduler Scheduler;
		ecs::Entity Hail;

		World(const char *name, scene::ParticleResponse response) : Store(name) {
			physics::PreparePhysicsWorld(Store);
			Ground(Store);
			physics::SyncBroadphase(Store);
			Hail = Field(Store, response);
			game::RegisterPhysicalParticleSystem(Scheduler);
		}

		void Run(int ticks) {
			for (int tick = 0; tick < ticks; ++tick)
				Scheduler.Tick(Store, 1.0f / 60.0f);
		}
	};
}

TEST_CASE("physical particles land on anchored parts and report each hit", "[game][particles]") {
	Register();
	World world("particles.bounce", scene::ParticleResponse::Bounce);
	world.Run(180);

	const auto *log = world.Store.Resource<scene::ParticleCollisionLog>();
	REQUIRE(log);
	REQUIRE_FALSE(log->Records.empty());
	for (const auto &hit : log->Records) {
		CHECK(hit.Field == world.Hail);
		CHECK(hit.Position.Y == Approx(1.0f).margin(0.01f));
		CHECK(hit.Normal.Y == Approx(1.0f));
		CHECK(hit.Velocity.Y < 0.0f);
	}
	// Nothing falls through the slab, and something has bounced to rest on it.
	bool resting = false;
	for (const auto &particle : Particles(world.Store)) {
		CHECK(particle.Position[1] >= 65536);
		resting = resting || particle.Resting;
	}
	CHECK(resting);

	std::vector<scene::DrawInstance> rows;
	scene::AppendPhysicalParticleRows(world.Store, rows);
	CHECK(rows.size() == Particles(world.Store).size());
}

TEST_CASE("physical particle responses stick or remove at the hit", "[game][particles]") {
	Register();
	World stick("particles.stick", scene::ParticleResponse::Stick);
	stick.Run(120);
	size_t resting = 0;
	for (const auto &particle : Particles(stick.Store))
		if (particle.Resting) {
			++resting;
			CHECK(particle.Velocity == std::array<int64_t, 3>{});
		}
	CHECK(resting > 0);

	World die("particles.die", scene::ParticleResponse::Die);
	die.Run(120);
	REQUIRE(die.Store.Resource<scene::ParticleCollisionLog>());
	for (const auto &particle : Particles(die.Store)) {
		CHECK_FALSE(particle.Resting);
		CHECK(particle.Position[1] > 65536);
	}
}

TEST_CASE("two worlds stepping one field hold the same particles", "[game][particles]") {
	Register();
	World first("particles.first", scene::ParticleResponse::Bounce);
	World second("particles.second", scene::ParticleResponse::Bounce);
	first.Run(150);
	second.Run(150);
	REQUIRE_FALSE(Particles(first.Store).empty());
	CHECK(Same(Particles(first.Store), Particles(second.Store)));
}

TEST_CASE("a destroyed field keeps its particles until the last one is gone", "[game][particles]") {
	Register();
	World world("particles.dying", scene::ParticleResponse::Stick);
	world.Run(120);
	REQUIRE_FALSE(Particles(world.Store).empty());
	world.Store.Destroy(world.Hail);
	world.Run(1);
	const auto *records = world.Store.Resource<scene::PhysicalParticleWorld>();
	REQUIRE(records->Records.size() == 1);
	CHECK(records->Records.front().Dying);
	REQUIRE(world.Store.Resource<scene::ParticleCollisionLog>());
	const uint64_t before = world.Store.Resource<scene::ParticleCollisionLog>()->NextSequence;
	world.Run(60 * 5);
	CHECK(world.Store.Resource<scene::PhysicalParticleWorld>()->Records.empty());
	// No instance is left to listen, so a dying field reports nothing.
	CHECK(world.Store.Resource<scene::ParticleCollisionLog>()->NextSequence == before);
}

TEST_CASE("a late joiner on a weather clock replays to the same particles", "[game][particles][weather]") {
	Register();
	const auto build = [](ecs::Store &store, ecs::Scheduler &scheduler) {
		physics::PreparePhysicsWorld(store);
		Ground(store);
		physics::SyncBroadphase(store);
		const auto storm = store.Create("Storm");
		scene::WeatherCompute controls;
		controls.BaseTemperatureCelsius = -20;
		controls.TemperatureVariationCelsius = 0;
		controls.IceFraction = 1;
		controls.StormIntensity = 1;
		controls.HailIntensity = 1;
		controls.PrecipitationRate = 4;
		controls.LiftMetresPerSecond = 0;
		controls.TurbulenceMetresPerSecond = 0;
		store.Set(storm, controls);
		const auto field = Field(store, scene::ParticleResponse::Bounce);
		store.Set(field, scene::PhysicalParticleSource{storm, 10.0f, scene::ParticleChannel::All});
		weather::RegisterWeatherClockSystem(scheduler);
		game::RegisterWeatherGroundSystem(scheduler);
	};

	ecs::Store early("particles.early");
	ecs::Scheduler earlySystems;
	build(early, earlySystems);
	game::RegisterPhysicalParticleSystem(earlySystems);

	ecs::Store late("particles.late");
	ecs::Scheduler lateSystems;
	build(late, lateSystems);

	for (int tick = 0; tick < 400; ++tick) {
		earlySystems.Tick(early, 0.05f);
		lateSystems.Tick(late, 0.05f);
	}
	game::StepPhysicalParticles(late);

	REQUIRE_FALSE(Particles(early).empty());
	CHECK(Same(Particles(early), Particles(late)));
	// The replay reaches the present only on its last step, so it reports
	// at most that step's hits.
	const auto *lateLog = late.Resource<scene::ParticleCollisionLog>();
	const auto *earlyLog = early.Resource<scene::ParticleCollisionLog>();
	REQUIRE(earlyLog);
	CHECK((lateLog ? lateLog->Records.size() : 0) < earlyLog->Records.size());
}
