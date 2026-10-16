// Real collider queries through linked panes. Scene setup and semantic checks
// stay outside the measured batches; no timing drives the deterministic world.

#include "WorldResource.hpp"

#include <engine/core/HeapProfile.hpp>
#include <engine/core/Name.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/physics/Broadphase.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/physics/Query.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

TEST_SUITE_ID("engine.physics.bench.portal-queries")
TEST_DEPENDS("engine.physics.query")

namespace {
	using namespace engine;
	constexpr size_t RAYS = 64;
	constexpr size_t BATCHES = 32;
	constexpr size_t SAMPLE_CAPACITY = 4096;
	constexpr std::array<const char *, 5> CASES{
		"empty", "simple", "complex_rotated_scaled", "blocked", "short_of_aperture"
	};
	struct Fixture {
		std::unique_ptr<ecs::Store> Store;
		std::array<core::Ray, RAYS> Rays;
		std::array<ecs::Entity, RAYS> Expected;
		size_t Pairs = 0;
		size_t Colliders = 0;
		float Distance = 5;
		float ExpectedDistance = 2.5f;
	};
	struct Measurement {
		std::array<uint64_t, SAMPLE_CAPACITY> Nanoseconds{};
		uint64_t Batches = 0;
		uint64_t Hits = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t AllocatedBlocks = 0;
		int64_t LiveByteChange = 0;
	};

	Fixture MakeFixture(size_t caseIndex) {
		Fixture fixture;
		fixture.Store = std::make_unique<ecs::Store>("benchmark.portal.queries");
		auto &store = *fixture.Store;
		physics::PreparePhysicsWorld(store, 2);
		scene::RegisterSceneClasses();
		fixture.Pairs = caseIndex == 0 ? 0 : caseIndex == 2 ? 32 : 1;
		const auto part = [&](const core::CFrame &frame, core::Vector3 extent, bool instance) {
			const auto entity =
				instance ? store.CreateInstance(ecs::Classes::Find(core::Name("Part")), "QueryPart")
						 : store.Create();
			store.Set(entity, scene::Transform{frame});
			store.Set(entity, scene::Bounds{extent});
			scene::Collider collider;
			collider.Extent = extent;
			store.Set(entity, collider);
			++fixture.Colliders;
			return entity;
		};
		std::array<ecs::Entity, 32> expected{};
		for (size_t pair = 0; pair < fixture.Pairs; ++pair) {
			const float scale = caseIndex == 2 && pair % 2 == 0 ? 2.0f : 1.0f;
			const float x = float(pair) * 32;
			auto farFrame = core::CFrame::Angles(0, caseIndex == 2 && pair % 2 ? 1.5707963268f : 0, 0);
			farFrame.Position = {4096 + float(pair) * 64, 0, 0};
			const auto nearPane = part(core::CFrame({x, 0, 0}), {8, 4.5f, .2f}, true);
			const auto farPane = part(farFrame, {8 * scale, 4.5f * scale, .2f * scale}, true);
			const auto link = [&](ecs::Entity from, ecs::Entity to) {
				const auto camera =
					store.CreateInstance(ecs::Classes::Find(core::Name("SurfaceCamera")), "Hole");
				store.Set(camera, scene::SurfaceCamera{});
				store.Set(camera, scene::Portal{to});
				store.SetParent(camera, from);
			};
			link(nearPane, farPane);
			link(farPane, nearPane);
			auto targetFrame = farFrame;
			targetFrame.Position = farFrame.PointToWorldSpace({0, 0, 1.6f * scale});
			expected[pair] = part(targetFrame, {4 * scale, 4 * scale, .5f * scale}, false);
			if (caseIndex == 3) {
				expected[pair] = part(core::CFrame({x, 0, .5f}), {4, 4, .1f}, false);
				fixture.ExpectedDistance = .4f;
			}
			if (caseIndex == 4) {
				expected[pair] = nearPane;
				fixture.Distance = 1;
				fixture.ExpectedDistance = .8f;
			}
		}
		if (caseIndex == 2) {
			for (size_t index = 0; index < 4096; ++index) {
				const auto entity = part(
					core::CFrame({float(index % 64) * 2, 30, float(index / 64) * 2}), {.4f, .4f, .4f}, false
				);
				store.GetMutable<scene::Collider>(entity)->Shape = scene::ShapeKind(index % 3);
			}
		}
		scene::OpenPortals(store);
		physics::SyncBroadphase(store);
		physics::BroadPhase(store);
		for (size_t index = 0; index < RAYS; ++index) {
			const size_t pair = fixture.Pairs ? index % fixture.Pairs : 0;
			fixture.Rays[index] = {{float(pair) * 32 + float(index % 5) * .1f, 0, 1}, {0, 0, -1}};
			fixture.Expected[index] = expected[pair];
			const auto hit = physics::RaycastThroughPortals(store, fixture.Rays[index], fixture.Distance);
			if (hit.has_value() != (fixture.Pairs != 0) ||
				(hit && (hit->Owner != fixture.Expected[index] ||
						 std::abs(hit->Distance - fixture.ExpectedDistance) > .003f)))
				throw std::runtime_error("portal query benchmark failed its analytic owner/distance oracle");
		}
		return fixture;
	}

	struct Benchmarks {
		std::array<Fixture, CASES.size()> Fixtures;
		std::array<Measurement, CASES.size()> Measurements;
	};
	void Print();
	Benchmarks &Scenarios() {
		static Benchmarks scenarios{
			{MakeFixture(0), MakeFixture(1), MakeFixture(2), MakeFixture(3), MakeFixture(4)}, {}
		};
		static const int registered = std::atexit(Print);
		if (registered != 0) throw std::runtime_error("could not register portal query benchmark report");
		return scenarios;
	}
	void Print() {
		const auto &scenarios = Scenarios();
		for (size_t index = 0; index < CASES.size(); ++index) {
			const auto &measured = scenarios.Measurements[index];
			if (!measured.Batches) continue;
			auto samples = measured.Nanoseconds;
			const size_t count = std::min<uint64_t>(measured.Batches, samples.size());
			std::sort(samples.begin(), samples.begin() + count);
			const auto percentile = [&](size_t percent) {
				return samples[(percent * (count - 1) + 99) / 100];
			};
			const auto &fixture = scenarios.Fixtures[index];
			const auto memory = physics::PreparedWorld(*fixture.Store)->MemoryStats().Total();
			std::cout << "# portal-query case=" << CASES[index]
					  << " backend=cpu portal_pairs=" << fixture.Pairs << " colliders=" << fixture.Colliders
					  << " max_distance=" << fixture.Distance << " rays_per_batch=" << RAYS
					  << " requests=" << measured.Batches * RAYS << " hits=" << measured.Hits
					  << " batch_ns_p50=" << percentile(50) << " batch_ns_p95=" << percentile(95)
					  << " batch_ns_p99=" << percentile(99) << " latency_batches=" << count
					  << " latency_window=latest4096_including_runner_warmup"
					  << " physics_live_bytes=" << memory.LiveBytes
					  << " physics_retained_bytes=" << memory.RetainedBytes
					  << " oracle=analytic_owner_distance tolerance_metres=0.003 heap_tracking="
					  << (core::HeapProfile::IsCompiledIn() ? "available" : "unavailable");
			if (core::HeapProfile::IsCompiledIn())
				std::cout << " allocated_bytes=" << measured.AllocatedBytes
						  << " allocated_blocks=" << measured.AllocatedBlocks
						  << " live_byte_change=" << measured.LiveByteChange;
			std::cout << " gpu_memory=not_applicable\n";
		}
	}
	void Run(size_t caseIndex) {
		auto &scenarios = Scenarios();
		auto &fixture = scenarios.Fixtures[caseIndex];
		auto &measured = scenarios.Measurements[caseIndex];
		const auto before = core::HeapProfile::Totals();
		for (size_t batch = 0; batch < BATCHES; ++batch) {
			const auto start = std::chrono::steady_clock::now();
			for (const auto &ray : fixture.Rays) {
				const auto hit = physics::RaycastThroughPortals(*fixture.Store, ray, fixture.Distance);
				measured.Hits += hit.has_value();
				testing::Consume(hit);
			}
			measured.Nanoseconds[measured.Batches++ % SAMPLE_CAPACITY] = uint64_t(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
					.count()
			);
		}
		const auto after = core::HeapProfile::Totals();
		measured.AllocatedBytes += after.TotalBytes - before.TotalBytes;
		measured.AllocatedBlocks += after.TotalBlocks - before.TotalBlocks;
		measured.LiveByteChange += after.LiveBytes - before.LiveBytes;
	}
}

BENCH("Portal ray · empty world · 64 rays per batch", BATCHES) {
	Run(0);
}
BENCH("Portal ray · one linked pair · 64 rays per batch", BATCHES) {
	Run(1);
}
BENCH("Portal ray · 32 rotated/scaled pairs and 4096 distractors · 64 rays per batch", BATCHES) {
	Run(2);
}
BENCH("Portal ray · near wall wins · 64 rays per batch", BATCHES) {
	Run(3);
}
BENCH("Portal ray · reach stops before aperture · 64 rays per batch", BATCHES) {
	Run(4);
}
