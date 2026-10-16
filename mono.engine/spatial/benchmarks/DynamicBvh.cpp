#include <engine/core/HeapProfile.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Bench.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.spatial.bench.dynamicbvh")

using engine::core::AABB;
using engine::core::Ray;
using engine::core::Vector3;
using engine::spatial::DynamicBvh;
using engine::spatial::LayerMask;
using engine::spatial::OverlapBox;
using engine::spatial::Proxy;
using engine::spatial::Raycast;
using engine::spatial::RaycastAll;
using engine::testing::Consume;

namespace {
	std::vector<Proxy> Scene(size_t count, float offset = 0.0f) {
		std::vector<Proxy> proxies;
		proxies.reserve(count);
		for (size_t index = 0; index < count; index++) {
			const float x = static_cast<float>(index % 64) * 2.0f + offset;
			const float z = static_cast<float>(index / 64) * 2.0f;
			proxies.push_back(
				Proxy{
					static_cast<uint64_t>(index),
					AABB::FromCentre(Vector3{x, 0.0f, z}, Vector3{0.4f, 0.4f, 0.4f}),
					LayerMask::All()
				}
			);
		}
		return proxies;
	}
}

namespace {
	constexpr size_t RAYS_PER_BATCH = 64;
	// Fixed batches amortise clock reads; retained samples remain bounded for long runner sweeps.
	constexpr size_t RAY_BATCHES = 32;
	constexpr size_t LATENCY_BATCH_CAPACITY = 4096;
	constexpr std::array<const char *, 8> RAY_CASES{
		"simple_diagonal_nearest",
		"complex_diagonal_nearest",
		"simple_short_nearest",
		"complex_short_nearest",
		"complex_diagonal_miss",
		"simple_all_hits",
		"complex_all_hits",
		"complex_all_hits_overflow"
	};

	struct RayFixture {
		DynamicBvh Tree;
		std::array<Ray, RAYS_PER_BATCH> Diagonal;
		std::array<Ray, RAYS_PER_BATCH> Short;
		std::array<Ray, RAYS_PER_BATCH> Miss;
		float LongDistance = 0;
		size_t Side = 0;
	};

	// The extra first-cell proxy makes equal-distance id ordering part of every hit workload.
	RayFixture MakeRayFixture(size_t side) {
		RayFixture fixture;
		fixture.Side = side;
		fixture.LongDistance = float(side) * 3.0f;
		std::vector<Proxy> proxies;
		proxies.reserve(side * side + 1);
		for (size_t row = 0; row < side; ++row) {
			for (size_t column = 0; column < side; ++column) {
				proxies.push_back(
					{1000 + row * side + column,
					 AABB::FromCentre({float(column) * 2, 0, float(row) * 2}, {.4f, .4f, .4f}),
					 LayerMask::All()}
				);
			}
		}
		proxies.push_back({7, proxies.front().Bounds, LayerMask::All()});
		fixture.Tree.Rebuild(proxies);
		const Vector3 diagonal = Vector3{1, 0, 1}.Unit();
		for (size_t index = 0; index < RAYS_PER_BATCH; ++index) {
			const float height = -.3f + .6f * float(index) / float(RAYS_PER_BATCH - 1);
			fixture.Diagonal[index] = {{-2, height, -2}, diagonal};
			fixture.Short[index] = {{-2, height, float(index % side) * 2}, Vector3::XAxis};
			// Its segment bounds cover the grid, but its line passes between diagonal boxes.
			fixture.Miss[index] = {{-2, height, -1}, diagonal};
		}
		std::array<engine::core::RayHit, 128> hits{};
		const auto all =
			RaycastAll(fixture.Tree, fixture.Diagonal[0], fixture.LongDistance, LayerMask::All(), hits);
		const auto nearest = Raycast(fixture.Tree, fixture.Diagonal[0], fixture.LongDistance);
		if (all.Overflowed || all.Written != side + 1 || !nearest || nearest->Id != 7 || hits[0].Id != 7 ||
			hits[1].Id != 1000 || Raycast(fixture.Tree, fixture.Miss[0], fixture.LongDistance))
			throw std::runtime_error("dynamic BVH ray fixture did not produce its declared workload");
		return fixture;
	}

	struct RayMeasurements {
		std::array<uint64_t, LATENCY_BATCH_CAPACITY> BatchNanoseconds{};
		uint64_t Batches = 0;
		uint64_t Hits = 0;
		uint64_t Overflows = 0;
		uint64_t AllocatedBytes = 0;
		uint64_t AllocatedBlocks = 0;
		int64_t LiveByteChange = 0;
	};

	struct RayBenchmarks {
		std::array<RayFixture, 2> Scenes;
		std::array<RayMeasurements, RAY_CASES.size()> Measurements;
	};

	void PrintRayMeasurements();

	RayBenchmarks &RayFixtures() {
		// Setup and output happen in runner warmup and at exit, outside measured query batches.
		static RayBenchmarks fixtures{{MakeRayFixture(8), MakeRayFixture(64)}, {}};
		static const int registered = std::atexit(PrintRayMeasurements);
		if (registered != 0) throw std::runtime_error("could not register dynamic BVH ray report");
		return fixtures;
	}

	void PrintRayMeasurements() {
		const auto &fixtures = RayFixtures();
		for (size_t caseIndex = 0; caseIndex < RAY_CASES.size(); ++caseIndex) {
			const auto &measured = fixtures.Measurements[caseIndex];
			if (!measured.Batches) continue;
			const bool simple = caseIndex == 0 || caseIndex == 2 || caseIndex == 5;
			const auto &fixture = fixtures.Scenes[simple ? 0 : 1];
			const auto stats = fixture.Tree.Stats();
			auto latencies = measured.BatchNanoseconds;
			const size_t count = std::min<uint64_t>(measured.Batches, latencies.size());
			std::sort(latencies.begin(), latencies.begin() + count);
			const auto percentile = [&](size_t percent) {
				return latencies[(percent * (count - 1) + 99) / 100];
			};
			std::cout << "# dynamic-bvh-rays case=" << RAY_CASES[caseIndex]
					  << " backend=cpu scene_side=" << fixture.Side << " proxies=" << stats.ProxyCount
					  << " spacing_metres=2 half_extent_metres=0.4 tied_proxies=2 max_distance_metres="
					  << (caseIndex == 2 || caseIndex == 3 ? 8.0f : fixture.LongDistance)
					  << " output_capacity="
					  << (caseIndex < 5	   ? 1
						  : caseIndex == 7 ? 8
										   : 128)
					  << " rays_per_batch=" << RAYS_PER_BATCH << " batches=" << measured.Batches
					  << " ray_requests=" << measured.Batches * RAYS_PER_BATCH
					  << " written_hits=" << measured.Hits << " overflow_queries=" << measured.Overflows
					  << " latency_batches=" << count << " batch_ns_p50=" << percentile(50)
					  << " batch_ns_p95=" << percentile(95) << " batch_ns_p99=" << percentile(99)
					  << " tree_live_bytes=" << stats.LiveBytes
					  << " tree_retained_bytes=" << stats.RetainedBytes << " heap_tracking="
					  << (engine::core::HeapProfile::IsCompiledIn() ? "available" : "unavailable");
			if (engine::core::HeapProfile::IsCompiledIn())
				std::cout << " allocated_bytes=" << measured.AllocatedBytes
						  << " allocated_blocks=" << measured.AllocatedBlocks
						  << " live_byte_change=" << measured.LiveByteChange;
			std::cout << " latency_window=latest_4096_batches_including_runner_warmup"
					  << " work=reported_hits_not_node_visits heap_coverage=cxx_new_delete_when_available\n";
		}
	}

	// The runner reports ns per 64-ray batch; supplemental percentiles include two clock reads per batch.
	void MeasureRays(size_t caseIndex) {
		auto &fixtures = RayFixtures();
		const bool simple = caseIndex == 0 || caseIndex == 2 || caseIndex == 5;
		const auto &fixture = fixtures.Scenes[simple ? 0 : 1];
		auto &measured = fixtures.Measurements[caseIndex];
		const bool shortRays = caseIndex == 2 || caseIndex == 3;
		const auto &rays = shortRays ? fixture.Short : caseIndex == 4 ? fixture.Miss : fixture.Diagonal;
		const float distance = shortRays ? 8.0f : fixture.LongDistance;
		std::array<engine::core::RayHit, 128> hits{};
		const auto heapBefore = engine::core::HeapProfile::Totals();
		for (size_t batch = 0; batch < RAY_BATCHES; ++batch) {
			const auto started = std::chrono::steady_clock::now();
			for (const Ray &ray : rays) {
				if (caseIndex < 5) {
					const auto nearest = Raycast(fixture.Tree, ray, distance);
					measured.Hits += nearest.has_value();
					Consume(nearest);
					continue;
				}
				const auto output = std::span(hits).first(caseIndex == 7 ? 8 : 128);
				const auto found = RaycastAll(fixture.Tree, ray, distance, LayerMask::All(), output);
				measured.Hits += found.Written;
				measured.Overflows += found.Overflowed;
				Consume(found);
				Consume(hits);
			}
			const auto elapsed = std::chrono::steady_clock::now() - started;
			measured.BatchNanoseconds[measured.Batches++ % LATENCY_BATCH_CAPACITY] =
				uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
		}
		const auto heapAfter = engine::core::HeapProfile::Totals();
		measured.AllocatedBytes += heapAfter.TotalBytes - heapBefore.TotalBytes;
		measured.AllocatedBlocks += heapAfter.TotalBlocks - heapBefore.TotalBlocks;
		measured.LiveByteChange += heapAfter.LiveBytes - heapBefore.LiveBytes;
	}
}

BENCH("Raycast nearest diagonal · 65 proxies · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(0);
}
BENCH("Raycast nearest diagonal · 4097 proxies · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(1);
}
BENCH("Raycast nearest short · 65 proxies · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(2);
}
BENCH("Raycast nearest short · 4097 proxies · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(3);
}
BENCH("Raycast diagonal miss · 4097 proxies · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(4);
}
BENCH("RaycastAll diagonal · 65 proxies · 128 hits · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(5);
}
BENCH("RaycastAll diagonal · 4097 proxies · 128 hits · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(6);
}
BENCH("RaycastAll diagonal · 4097 proxies · 8 hits · 64 rays per batch", RAY_BATCHES) {
	MeasureRays(7);
}

BENCH("Sync contained jitter · 4000 colliders", 100) {
	DynamicBvh tree;
	std::vector<Proxy> proxies = Scene(4000);
	tree.Rebuild(proxies);
	for (int pass = 0; pass < 100; pass++) {
		for (Proxy &proxy : proxies) {
			proxy.Bounds.Minimum.X += 0.01f;
			proxy.Bounds.Maximum.X += 0.01f;
		}
		Consume(tree.Sync(proxies, tree.Preflight(proxies)));
	}
}

BENCH("Sync few escaped leaves · 4000 colliders", 100) {
	DynamicBvh tree;
	std::vector<Proxy> proxies = Scene(4000);
	tree.Rebuild(proxies);
	for (int pass = 0; pass < 100; pass++) {
		proxies[static_cast<size_t>(pass % 4)].Bounds.Minimum.X += 1.0f;
		proxies[static_cast<size_t>(pass % 4)].Bounds.Maximum.X += 1.0f;
		Consume(tree.Sync(proxies, tree.Preflight(proxies)));
	}
}

BENCH("Exact pairs after alternating 4 and 40 escapes · 4000 colliders", 100) {
	DynamicBvh tree;
	std::vector<Proxy> proxies = Scene(4000);
	tree.Rebuild(proxies);
	for (int pass = 0; pass < 100; pass++) {
		const size_t escaped = pass % 2 == 0 ? 4 : 40;
		for (size_t index = 0; index < escaped; index++) {
			proxies[index].Bounds.Minimum.X += 1.0f;
			proxies[index].Bounds.Maximum.X += 1.0f;
		}
		Consume(tree.Sync(proxies, tree.Preflight(proxies)));
		size_t pairs = 0;
		tree.ForEachOverlappingPair([&pairs](const Proxy &, const Proxy &) {
			pairs++;
			return true;
		});
		Consume(pairs);
	}
}

BENCH("Rebuild all movers · 4000 colliders", 50) {
	DynamicBvh tree;
	for (int pass = 0; pass < 50; pass++) {
		tree.Rebuild(Scene(4000, static_cast<float>(pass)));
		Consume(tree.Stats().Height);
	}
}

BENCH("Clustered overlap · 4000 colliders", 200) {
	DynamicBvh tree;
	tree.Rebuild(Scene(4000));
	uint64_t found[4096];
	for (int pass = 0; pass < 200; pass++) {
		Consume(OverlapBox(
					tree,
					AABB{Vector3{-1.0f, -1.0f, -1.0f}, Vector3{130.0f, 1.0f, 130.0f}},
					LayerMask::All(),
					found
		)
					.Written);
	}
}

BENCH("Rebuild grow and shrink · 4000 colliders", 50) {
	DynamicBvh tree;
	for (int pass = 0; pass < 50; pass++) {
		tree.Rebuild(Scene(1000));
		tree.Rebuild(Scene(4000));
		Consume(tree.Stats().RetainedBytes);
	}
}
