// Host weather queries and registered preparation passes against real collider storage.
#include <engine/collision/TriangleMesh.hpp>
#include <engine/core/HeapProfile.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/game/WeatherGround.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/PhysicsWorld.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/testing/Bench.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

TEST_SUITE_ID("engine.game.bench.weatherground")
TEST_DEPENDS("engine.game.weatherground")

namespace {
	using namespace engine;
	using Clock = std::chrono::steady_clock;
	constexpr size_t CALLS = 64, CAPACITY = 512;
	void Require(bool okay, const char *message) {
		if (!okay) throw std::runtime_error(message);
	}
	bool Flag(const char *name) {
		const char *raw = std::getenv(name);
		if (!raw) return false;
		const std::string_view value(raw);
		Require(value == "0" || value == "1", "weather benchmark flags require 0 or 1");
		return value == "1";
	}
	size_t Option(const char *name, size_t fallback, size_t low, size_t high) {
		const char *raw = std::getenv(name);
		if (!raw) return fallback;
		const std::string_view text(raw);
		size_t value = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
		Require(
			result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= low &&
				value <= high,
			"weather benchmark option outside bounded range"
		);
		return value;
	}
	double Counter(const char *name) {
		const auto found = core::Metrics::Get(name);
		return found ? found->Value : 0;
	}
	struct Fixture {
		ecs::Store Store{"bench.weather.ground"};
		ecs::Entity Ground, Provider;
		ecs::Scheduler Preparation, Runtime;
		size_t Colliders = 0;
		float Top = 64;
		uint64_t IndexBeforePreparation = 0;
		Fixture(size_t count, bool terrain = false, bool impact = false) {
			scene::RegisterSceneClasses();
			weather::RegisterWeatherRuntimeComponents();
			physics::PreparePhysicsWorld(Store, 4);
			game::RegisterWeatherGroundSystem(Preparation);
			// Compile ordering without running either preparation phase.
			Preparation.RunPhases(Store, ecs::Phase::Input, ecs::Phase::Input);
			weather::RegisterWeatherClockSystem(Runtime);
			game::RegisterWeatherGroundSystem(Runtime);
			if (count) {
				auto frame = terrain || impact ? core::CFrame({0, impact ? 31.0f : 63.0f, 0})
											   : core::CFrame::Angles(0, 0, .2f);
				if (!terrain && !impact) frame.Position = {0, 10, 0};
				Ground =
					Part(frame, terrain || impact ? core::Vector3{512, 1, 512} : core::Vector3{16, .5f, 16});
				Part(core::CFrame({0, terrain || impact ? 180.0f : 20.0f, 0}), {16, .5f, 16}, true);
				if (count == 8192 && !terrain && !impact) {
					scene::CollisionShapes shapes;
					const std::array<core::Vector3, 4> vertices{
						{{-4, 0, -4}, {4, 0, -4}, {4, 0, 4}, {-4, 0, 4}}
					};
					const std::array<uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
					shapes.SetMesh(
						core::Name("weather.patch"), collision::BuildTriangleMesh(vertices, indices)
					);
					Store.SetResource(shapes);
					const auto patch = Part(core::CFrame({50, 15, 0}), {4, .5f, 4});
					auto collider = *Store.Get<scene::Collider>(patch);
					collider.Shape = scene::ShapeKind::Mesh;
					collider.Geometry = core::Name("weather.patch");
					Store.Set(patch, collider);
				}
				while (Colliders < count) {
					const size_t index = Colliders;
					auto pose = core::CFrame::Angles(.2f, .3f, -.1f);
					pose.Position = {256 + float(index % 128) * 8, -20, 256 + float(index / 128) * 8};
					const auto entity = Part(pose, {1, 1, 1});
					if (index % 4 == 0) Store.Set(entity, scene::Motion{});
				}
			}
			physics::SyncBroadphase(Store);
			const auto *world = Store.Resource<physics::PhysicsWorld>();
			Require(
				world->StaticColliders() + world->DynamicColliders() == count,
				"weather fixture actual collider count differs"
			);
			if (terrain || impact) {
				Provider = Store.Create("Storm");
				scene::WeatherCompute controls;
				controls.BaseHeightMetres = impact ? 256 : 0;
				controls.CellSizeMetres = 64;
				controls.ThicknessMetres = 256;
				controls.ExtentMetres = {256, 256, 256};
				controls.WindMetresPerSecond = {};
				controls.LiftMetresPerSecond = 0;
				controls.TurbulenceMetresPerSecond = 0;
				controls.ShearMetresPerSecond = 0;
				controls.TemperatureVariationCelsius = 0;
				controls.PressureVariationHectopascals = 0;
				controls.GroundVapourFluxKilogramsPerSquareMetreSecond = .0001f;
				if (impact) {
					controls.WindMetresPerSecond.Y = 8;
					controls.BaseTemperatureCelsius = -20;
					controls.IceFraction = 1;
					controls.StormIntensity = 1;
					controls.LightningRate = 1;
					controls.HailIntensity = 1;
				} else
					controls.Mode = scene::WeatherMode::Stateful;
				Store.Set(Provider, controls);
			} else
				game::InstallWeatherGround(Store);
		}
		ecs::Entity Part(const core::CFrame &frame, core::Vector3 half, bool trigger = false) {
			const auto entity = Store.Create();
			Store.Set(entity, scene::Transform{frame});
			scene::Collider collider;
			collider.Extent = half;
			collider.Trigger = trigger;
			Store.Set(entity, collider);
			++Colliders;
			return entity;
		}
		void MoveGround(float top) {
			Top = top;
			Store.Set(Ground, scene::Transform{core::CFrame({0, top - 1, 0})});
		}
		void VerifyTerrain() {
			Runtime.Tick(Store, 0);
			const auto *state = Store.Get<weather::StatefulWeatherState>(Provider);
			Require(
				state && state->Simulation.GroundHeightMetres.size() == 16,
				"weather terrain column count differs"
			);
			for (float height : state->Simulation.GroundHeightMetres)
				Require(std::abs(height - Top) < .001f, "weather terrain accepted height differs");
		}
	};
	struct Report {
		const char *Row = "";
		size_t Colliders = 0, Trials = 0, Warmups = 0, Calls = 0, Hits = 0;
		bool Memory = false, Heap = false;
		std::array<uint64_t, CAPACITY> Samples{};
		uint64_t AllocatedBytes = 0, AllocatedBlocks = 0;
		double HeightQueries = 0, TerrainSyncs = 0;
		core::HeapTotals LastHeap;
		physics::PhysicsMemoryStats LastPhysics;
	};
	std::vector<Report> Latest;
	void Print() {
		for (const auto &report : Latest) {
			std::cout << "weather-ground row=" << report.Row << " colliders=" << report.Colliders
					  << " samples=" << report.Trials << " warmups=" << report.Warmups
					  << " actual_caller_calls=" << report.Calls
					  << " actual_height_queries=" << report.HeightQueries
					  << " returned_height_hits=" << report.Hits
					  << " actual_terrain_syncs=" << report.TerrainSyncs
					  << " setup_oracles=excluded counter_scope=measured_calls "
						 "outer_body_includes_setup_oracles=true heap_compiled="
					  << report.Heap << " timing_valid=" << (!report.Memory && !report.Heap)
					  << " measurement_mode="
					  << (report.Memory ? "memory_diagnostic"
						  : report.Heap ? "heap_instrumented_latency"
										: "heap_off_latency");
			if (report.Memory) {
				if (report.Heap)
					std::cout << " allocated_bytes=" << report.AllocatedBytes
							  << " allocated_blocks=" << report.AllocatedBlocks
							  << " process_live_bytes=" << report.LastHeap.LiveBytes
							  << " process_live_blocks=" << report.LastHeap.LiveBlocks
							  << " process_lifetime_peak_bytes=" << report.LastHeap.PeakBytes
							  << " profiler_overhead_bytes=" << report.LastHeap.OverheadBytes;
				else
					std::cout
						<< " allocated_bytes=unavailable allocated_blocks=unavailable "
						   "process_live_bytes=unavailable process_live_blocks=unavailable "
						   "process_lifetime_peak_bytes=unavailable profiler_overhead_bytes=unavailable";
				const auto bytes = report.LastPhysics.Total();
				std::cout << " physics_live_bytes=" << bytes.LiveBytes
						  << " physics_retained_bytes=" << bytes.RetainedBytes
						  << " physics_scope=world_containers_excludes_ecs_and_mesh "
							 "heap_scope=sum_measured_windows\n";
				continue;
			}
			auto samples = report.Samples;
			std::sort(samples.begin(), samples.begin() + report.Trials);
			const auto rank = [&](size_t p) { return samples[(report.Trials * p + 99) / 100 - 1]; };
			double total = 0;
			for (size_t index = 0; index < report.Trials; ++index)
				total += double(report.Samples[index]);
			std::cout << " batch_p50_ns=" << rank(50) << " batch_p95_ns=" << rank(95)
					  << " batch_p99_ns=" << rank(99) << " batch_max_ns=" << samples[report.Trials - 1]
					  << " derived_mean_ns_per_caller=" << total / double(report.Calls)
					  << " quantile=nearest_rank individual_call_tails=not_measured tail_status="
					  << (report.Trials >= 100 ? "at_least100" : "insufficient_below100") << '\n';
		}
	}
	template <class Setup, class Call, class Verify>
	void Measure(
		const char *name,
		size_t colliders,
		size_t calls,
		size_t trials,
		size_t warmups,
		bool memory,
		Setup setup,
		Call call,
		Verify verify
	) {
		Report report;
		report.Row = name;
		report.Colliders = colliders;
		report.Trials = trials;
		report.Warmups = warmups;
		report.Memory = memory;
		report.Heap = core::HeapProfile::IsCompiledIn();
		for (size_t sample = 0; sample < trials + warmups; ++sample) {
			Fixture &fixture = setup(sample);
			const double queries = Counter("weather.ground.height_queries"),
						 syncs = Counter("weather.terrain.broadphase_syncs");
			core::HeapTotals before;
			if (memory && report.Heap) before = core::HeapProfile::Totals();
			const auto begin = Clock::now();
			call(fixture);
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
			core::HeapTotals after;
			if (memory && report.Heap) after = core::HeapProfile::Totals();
			const double queryDelta = Counter("weather.ground.height_queries") - queries;
			const double syncDelta = Counter("weather.terrain.broadphase_syncs") - syncs;
			const auto physicsMemory = fixture.Store.Resource<physics::PhysicsWorld>()->MemoryStats();
			const size_t hits = verify(fixture, queryDelta, syncDelta);
			if (sample < warmups) continue;
			report.Samples[sample - warmups] = static_cast<uint64_t>(elapsed);
			report.Calls += calls;
			report.Hits += hits;
			report.HeightQueries += queryDelta;
			report.TerrainSyncs += syncDelta;
			if (memory && report.Heap) {
				Require(after.DroppedScopes == before.DroppedScopes, "weather benchmark heap scopes dropped");
				report.AllocatedBytes += after.TotalBytes - before.TotalBytes;
				report.AllocatedBlocks += after.TotalBlocks - before.TotalBlocks;
				report.LastHeap = after;
			}
			report.LastPhysics = physicsMemory;
		}
		Latest.push_back(report);
	}
	void Run() {
		const bool enabled = Flag("MONO_WEATHER_GROUND_BENCH"), memory = Flag("MONO_WEATHER_GROUND_MEMORY");
		if (!enabled) {
			std::cout << "weather-ground skipped=opt_in_required\n";
			return;
		}
		const size_t trials = Option("MONO_WEATHER_GROUND_TRIALS", 101, 1, CAPACITY),
					 warmups = Option("MONO_WEATHER_GROUND_WARMUPS", 3, 0, 32);
		static const bool registered = [] {
			Require(std::atexit(Print) == 0, "weather report registration failed");
			return true;
		}();
		(void)registered;
		Latest.clear();
		Latest.reserve(7);
		for (size_t count : {0u, 8u, 1024u, 8192u}) {
			Fixture fixture(count);
			std::array<core::Vector3, CALLS> origins;
			std::array<float, CALLS> ranges;
			std::array<std::optional<float>, CALLS> expected{}, actual{};
			for (size_t index = 0; index < CALLS; ++index) {
				const size_t kind = index % 4;
				const float x = kind == 1					 ? -100.0f
								: kind == 3 && count == 8192 ? 50.0f
															 : float(index % 5) - 2;
				origins[index] = {x, 50, 0};
				ranges[index] = kind == 2 ? 10 : 100;
				if (count && kind != 1 && kind != 2)
					expected[index] =
						kind == 3 && count == 8192 ? 15.0f : 10 + (.5f + x * std::sin(.2f)) / std::cos(.2f);
			}
			const auto call = [&](Fixture &value) {
				const auto *query = value.Store.Resource<weather::WeatherGroundQuery>();
				Require(query && query->Height, "real weather height binding absent");
				for (size_t index = 0; index < CALLS; ++index)
					actual[index] = query->Height(value.Store, origins[index], ranges[index]);
			};
			const auto verify = [&](Fixture &, double queries, double) {
				Require(queries == CALLS, "weather actual query count differs");
				size_t hits = 0;
				for (size_t index = 0; index < CALLS; ++index) {
					Require(
						bool(actual[index]) == bool(expected[index]),
						"weather analytic height hit/miss differs"
					);
					if (actual[index]) {
						Require(
							std::abs(*actual[index] - *expected[index]) < .002f,
							"weather analytic plane height differs"
						);
						++hits;
					}
				}
				return hits;
			};
			call(fixture);
			(void)verify(fixture, CALLS, 0);
			Measure(
				"installed_height_binding",
				count,
				CALLS,
				trials,
				warmups,
				memory,
				[&](size_t) -> Fixture & { return fixture; },
				call,
				verify
			);
		}
		std::unique_ptr<Fixture> cold;
		Measure(
			"cold_registered_terrain_preparation",
			8,
			1,
			trials,
			warmups,
			memory,
			[&](size_t sample) -> Fixture & {
				cold = std::make_unique<Fixture>(8, true);
				cold->MoveGround(sample % 2 ? 128 : 64);
				auto controls = *cold->Store.Get<scene::WeatherCompute>(cold->Provider);
				controls.TerrainRevision += static_cast<uint32_t>(sample + 1);
				controls.DefinitionRevision += static_cast<uint32_t>(sample + 1);
				cold->Store.Set(cold->Provider, controls);
				return *cold;
			},
			[](Fixture &value) {
				value.Preparation.RunPhases(
					value.Store, ecs::Phase::PreSimulation, ecs::Phase::PreSimulation
				);
			},
			[](Fixture &value, double queries, double syncs) {
				Require(queries == 0 && syncs == 1, "weather cold preparation sync count differs");
				const auto *query = value.Store.Resource<weather::WeatherGroundQuery>();
				Require(query && query->Height, "timed weather preparation did not install its real binding");
				const auto height = query->Height(value.Store, {0, 250, 0}, 300);
				Require(
					height && std::abs(*height - value.Top) < .001f,
					"timed weather preparation height differs before runtime verification"
				);
				value.VerifyTerrain();
				return size_t{0};
			}
		);
		Fixture warm(8, true);
		warm.VerifyTerrain();
		warm.Preparation.RunPhases(warm.Store, ecs::Phase::PreSimulation, ecs::Phase::PreSimulation);
		Measure(
			"warm_registered_terrain_preparation",
			8,
			CALLS,
			trials,
			warmups,
			memory,
			[&](size_t) -> Fixture & { return warm; },
			[](Fixture &value) {
				for (size_t call = 0; call < CALLS; ++call)
					value.Preparation.RunPhases(
						value.Store, ecs::Phase::PreSimulation, ecs::Phase::PreSimulation
					);
			},
			[](Fixture &value, double queries, double syncs) {
				Require(queries == 0 && syncs == 0, "weather warm preparation rebuilt terrain");
				value.VerifyTerrain();
				return size_t{0};
			}
		);
		std::unique_ptr<Fixture> impact;
		Measure(
			"registered_impact_index_preparation",
			8,
			1,
			trials,
			warmups,
			memory,
			[&](size_t sample) -> Fixture & {
				impact = std::make_unique<Fixture>(8, false, true);
				impact->Store.AdvanceTick(0);
				weather::UpdateWeatherClocks(impact->Store);
				weather::UpdateWeatherRuntime(impact->Store);
				impact->Store.AdvanceTick(2);
				weather::UpdateWeatherClocks(impact->Store);
				impact->IndexBeforePreparation =
					impact->Store.Resource<physics::PhysicsWorld>()->StaticRebuilds();
				impact->MoveGround(sample % 2 ? 128 : 64);
				return *impact;
			},
			[](Fixture &value) {
				value.Preparation.RunPhases(value.Store, ecs::Phase::Animation, ecs::Phase::Animation);
			},
			[](Fixture &value, double, double) {
				Require(
					value.Store.Resource<physics::PhysicsWorld>()->StaticRebuilds() ==
						value.IndexBeforePreparation + 1,
					"weather impact preparation did not refresh its real static index"
				);
				weather::UpdateWeatherRuntime(value.Store);
				const auto *events = value.Store.Get<weather::WeatherEvents>(value.Provider);
				Require(events && !events->Events.empty(), "weather prepared impact produced no real events");
				for (const auto &event : events->Events)
					Require(
						std::abs(event.EndMetres.Y - value.Top) < .002f,
						"weather impact endpoint ignores prepared ground"
					);
				return size_t{0};
			}
		);
	}
}

BENCH("Weather ground | installed queries and actual registered preparation", 1) {
	Run();
}
