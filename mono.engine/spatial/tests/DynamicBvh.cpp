#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.spatial.dynamicbvh")
TEST_DEPENDS("engine.core.types.aabb")
TEST_DEPENDS("engine.core.types.ray")
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::AABB;
using engine::core::Ray;
using engine::core::Vector3;
using engine::spatial::DynamicBvh;
using engine::spatial::HashGrid;
using engine::spatial::LayerMask;
using engine::spatial::OverlapBox;
using engine::spatial::OverlapSphere;
using engine::spatial::Proxy;
using engine::spatial::Raycast;
using engine::spatial::RaycastAll;
using engine::spatial::ShapeCast;

namespace {
	Proxy Cube(uint64_t id, float x, LayerMask layers = LayerMask::All()) {
		return Proxy{id, AABB::FromCentre(Vector3{x, 0.0f, 0.0f}, Vector3{0.1f, 0.1f, 0.1f}), layers};
	}

	std::vector<uint64_t> Overlap(const DynamicBvh &tree) {
		std::array<uint64_t, 256> ids{};
		const auto result = OverlapBox(
			tree, AABB{Vector3{-10.0f, -1.0f, -1.0f}, Vector3{200.0f, 1.0f, 1.0f}}, LayerMask::All(), ids
		);
		REQUIRE_FALSE(result.Overflowed);
		return std::vector<uint64_t>{ids.begin(), ids.begin() + result.Written};
	}
}

TEST_CASE("a dynamic hierarchy reports tight bounds after contained jitter", "[dynamicbvh]") {
	DynamicBvh tree;
	std::array<Proxy, 2> proxies{Cube(7, 0.0f), Cube(3, 1.0f)};
	tree.Rebuild(proxies);
	proxies[0] = Cube(7, 0.1f);
	const auto preflight = tree.Preflight(proxies);
	REQUIRE(preflight.Compatible);
	REQUIRE(preflight.EscapedLeaves == 0);
	REQUIRE(tree.Sync(proxies, preflight));
	CHECK(tree.Stats().ReinsertedLeaves == 0);
	CHECK(Overlap(tree) == std::vector<uint64_t>{3, 7});
}

TEST_CASE("an escaped leaf refits while a positional reorder refuses sync", "[dynamicbvh]") {
	DynamicBvh tree;
	std::array<Proxy, 2> proxies{Cube(1, 0.0f), Cube(2, 1.0f)};
	tree.Rebuild(proxies);
	proxies[0] = Cube(1, 2.0f);
	const auto escaped = tree.Preflight(proxies);
	REQUIRE(escaped.Compatible);
	REQUIRE(escaped.EscapedLeaves == 1);
	REQUIRE(tree.Sync(proxies, escaped));
	CHECK(tree.Stats().RefittedLeaves == 1);
	CHECK(tree.Stats().ReinsertedLeaves == 0);
	std::swap(proxies[0], proxies[1]);
	CHECK_FALSE(tree.Preflight(proxies).Compatible);
}

TEST_CASE("a dynamic sync plan rejects stale trees and a mismatched source span", "[dynamicbvh]") {
	DynamicBvh tree;
	std::array<Proxy, 2> proxies{Cube(1, 0.0f), Cube(2, 1.0f)};
	tree.Rebuild(proxies);
	const auto stale = tree.Preflight(proxies);
	tree.Rebuild(proxies);
	CHECK_FALSE(tree.Sync(proxies, stale));
	const auto current = tree.Preflight(proxies);
	const std::array<Proxy, 2> copy = proxies;
	CHECK_FALSE(tree.Sync(copy, current));
}

TEST_CASE("dynamic hierarchy raycasts all ray directions and uses tight bounds", "[dynamicbvh]") {
	DynamicBvh tree;
	const std::array<Proxy, 2> proxies{Cube(1, -3.0f), Cube(2, 3.0f, LayerMask::Only(1))};
	tree.Rebuild(proxies);
	REQUIRE(Raycast(tree, Ray{Vector3{0.0f, 0.0f, 0.0f}, -Vector3::XAxis}, 10.0f)->Id == 1u);
	REQUIRE(
		Raycast(tree, Ray{Vector3{0.0f, 0.0f, 0.0f}, Vector3::XAxis}, 10.0f, LayerMask::Only(1))->Id == 2u
	);
}

TEST_CASE("dynamic hierarchy keeps nearest ray hits and bounded output deterministic", "[dynamicbvh]") {
	DynamicBvh tree;
	const std::array<Proxy, 3> proxies{Cube(9, 2.0f), Cube(3, 2.0f), Cube(5, 4.0f)};
	tree.Rebuild(proxies);
	const Ray ray{Vector3{0.0f, 0.0f, 0.0f}, Vector3::XAxis};
	REQUIRE(Raycast(tree, ray, 10.0f)->Id == 3u);
	std::array<engine::core::RayHit, 2> hits{};
	const auto result = RaycastAll(tree, ray, 10.0f, LayerMask::All(), hits);
	REQUIRE(result.Overflowed);
	CHECK(hits[0].Id == 3u);
	CHECK(hits[1].Id == 9u);
}

TEST_CASE("dynamic hierarchy filters layers and supports sphere and swept-box candidates", "[dynamicbvh]") {
	DynamicBvh tree;
	const std::array<Proxy, 2> proxies{Cube(1, 0.0f, LayerMask::Only(0)), Cube(2, 2.0f, LayerMask::Only(1))};
	tree.Rebuild(proxies);
	std::array<uint64_t, 4> ids{};
	const auto sphere = OverlapSphere(tree, Vector3{2.0f, 0.0f, 0.0f}, 0.2f, LayerMask::Only(1), ids);
	REQUIRE(sphere.Written == 1);
	CHECK(ids[0] == 2u);
	const auto sweep = ShapeCast(
		tree,
		AABB::FromCentre(Vector3{-2.0f, 0.0f, 0.0f}, Vector3{0.1f, 0.1f, 0.1f}),
		Vector3{4.0f, 0.0f, 0.0f},
		LayerMask::Only(0),
		ids
	);
	REQUIRE(sweep.Written == 1);
	CHECK(ids[0] == 1u);
}

TEST_CASE("different update histories retain identical canonical overlap output", "[dynamicbvh]") {
	std::array<Proxy, 3> target{Cube(9, 0.0f), Cube(2, 1.0f), Cube(5, 2.0f)};
	DynamicBvh rebuilt;
	rebuilt.Rebuild(target);
	DynamicBvh moved;
	std::array<Proxy, 3> start{Cube(9, -0.1f), Cube(2, 0.9f), Cube(5, 1.9f)};
	moved.Rebuild(start);
	const auto preflight = moved.Preflight(target);
	REQUIRE(moved.Sync(target, preflight));
	CHECK(Overlap(rebuilt) == Overlap(moved));
}

TEST_CASE("balanced rebuild keeps an adversarial ordered tree shallow", "[dynamicbvh]") {
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 128; index++) {
		proxies.push_back(Cube(index, static_cast<float>(index)));
	}
	DynamicBvh tree;
	tree.Rebuild(proxies);
	CHECK(tree.Stats().Height <= 8);
	CHECK(Overlap(tree).size() == 128);
}

TEST_CASE("dynamic hierarchy retains storage after grow and shrink", "[dynamicbvh]") {
	DynamicBvh tree;
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 64; index++) {
		proxies.push_back(Cube(index, static_cast<float>(index)));
	}
	tree.Rebuild(proxies);
	const size_t retained = tree.Stats().RetainedBytes;
	proxies.resize(2);
	tree.Rebuild(proxies);
	CHECK(tree.Stats().ProxyCount == 2);
	CHECK(tree.Stats().RetainedBytes >= retained);
	tree.Clear();
	CHECK(tree.Stats().RetainedBytes >= retained);
}

TEST_CASE("nonfinite proxy keys have a deterministic total rebuild order", "[dynamicbvh]") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float infinity = std::numeric_limits<float>::infinity();
	const std::array<Proxy, 4> proxies{
		Cube(4, 4.0f),
		Proxy{1, AABB{Vector3{nan, 0.0f, 0.0f}, Vector3{nan, 1.0f, 1.0f}}, LayerMask::All()},
		Cube(2, 2.0f),
		Proxy{3, AABB{Vector3{infinity, 0.0f, 0.0f}, Vector3{infinity, 1.0f, 1.0f}}, LayerMask::All()},
	};
	DynamicBvh first;
	DynamicBvh second;
	first.Rebuild(proxies);
	second.Rebuild(proxies);
	CHECK(first.Stats().ProxyCount == proxies.size());
	CHECK(first.Stats().Height == second.Stats().Height);
	CHECK(Overlap(first) == Overlap(second));
}

TEST_CASE("dynamic hierarchy pair traversal matches a brute-force oracle", "[dynamicbvh]") {
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 48; index++) {
		const float x = static_cast<float>((index * 17) % 23) * 0.18f;
		const float y = static_cast<float>((index * 7) % 11) * 0.12f;
		proxies.push_back(
			Proxy{
				index,
				AABB::FromCentre(Vector3{x, y, 0.0f}, Vector3{0.16f, 0.16f, 0.16f}),
				LayerMask::Only(static_cast<uint32_t>(index % 3)),
			}
		);
	}
	DynamicBvh tree;
	tree.Rebuild(proxies);
	for (size_t index = 0; index < proxies.size(); index += 5) {
		proxies[index].Bounds.Minimum.X += 0.1f;
		proxies[index].Bounds.Maximum.X += 0.1f;
	}
	const auto preflight = tree.Preflight(proxies);
	REQUIRE(tree.Sync(proxies, preflight));

	std::vector<std::pair<uint64_t, uint64_t>> actual;
	tree.ForEachOverlappingPair([&actual](const Proxy &first, const Proxy &second) {
		actual.emplace_back(std::min(first.Id, second.Id), std::max(first.Id, second.Id));
		return true;
	});
	std::sort(actual.begin(), actual.end());
	CHECK(std::adjacent_find(actual.begin(), actual.end()) == actual.end());
	size_t stopped = 0;
	CHECK_FALSE(tree.ForEachOverlappingPair([&stopped](const Proxy &, const Proxy &) {
		stopped++;
		return false;
	}));
	CHECK(stopped == 1);

	std::vector<std::pair<uint64_t, uint64_t>> expected;
	for (size_t first = 0; first < proxies.size(); first++) {
		for (size_t second = first + 1; second < proxies.size(); second++) {
			if (proxies[first].Bounds.Overlaps(proxies[second].Bounds)) {
				expected.emplace_back(proxies[first].Id, proxies[second].Id);
			}
		}
	}
	CHECK(actual == expected);
}

TEST_CASE("dynamic hierarchy enumerates coincident pairs after every leaf reinserts", "[dynamicbvh]") {
	std::array<Proxy, 12> proxies{};
	for (uint64_t index = 0; index < proxies.size(); index++) {
		proxies[index] = Cube(index, 0.0f, LayerMask::Only(static_cast<uint32_t>(index % 3)));
	}
	DynamicBvh tree;
	tree.Rebuild(proxies);
	for (Proxy &proxy : proxies) {
		proxy.Bounds.Minimum.X += 1.0f;
		proxy.Bounds.Maximum.X += 1.0f;
	}
	const auto preflight = tree.Preflight(proxies);
	REQUIRE(preflight.EscapedLeaves == proxies.size());
	REQUIRE(tree.Sync(proxies, preflight));
	CHECK(tree.Stats().ReinsertedLeaves == proxies.size());

	std::vector<std::pair<uint64_t, uint64_t>> pairs;
	tree.ForEachOverlappingPair([&pairs](const Proxy &first, const Proxy &second) {
		pairs.emplace_back(std::min(first.Id, second.Id), std::max(first.Id, second.Id));
		return true;
	});
	std::sort(pairs.begin(), pairs.end());
	CHECK(pairs.size() == proxies.size() * (proxies.size() - 1) / 2);
	CHECK(std::adjacent_find(pairs.begin(), pairs.end()) == pairs.end());
}

TEST_CASE("predicted fat bounds reduce repeated linear and reversing reinserts", "[dynamicbvh]") {
	DynamicBvh tree;
	std::array<Proxy, 2> proxies{Cube(1, 0.0f), Cube(2, 10.0f)};
	tree.Rebuild(proxies);
	proxies[0] = Cube(1, 1.0f);
	REQUIRE(tree.Sync(proxies, tree.Preflight(proxies)));
	CHECK(tree.Stats().ReinsertedLeaves == 0);
	proxies[0] = Cube(1, 2.0f);
	REQUIRE(tree.Sync(proxies, tree.Preflight(proxies)));
	CHECK(tree.Stats().ReinsertedLeaves == 0);
	proxies[0] = Cube(1, 0.0f);
	REQUIRE(tree.Sync(proxies, tree.Preflight(proxies)));
	CHECK(tree.Stats().ReinsertedLeaves == 0);
	CHECK(Overlap(tree) == std::vector<uint64_t>{1, 2});
}

TEST_CASE("small refits replace fat bounds and cached pairs across long travel", "[dynamicbvh]") {
	std::array<Proxy, 9> proxies{
		Cube(1, 0.0f),
		Cube(2, 4.0f),
		Cube(3, 8.0f),
		Cube(4, 12.0f),
		Cube(5, 16.0f),
		Cube(6, 20.0f),
		Cube(7, 24.0f),
		Cube(8, 28.0f),
		Cube(9, 32.0f),
	};
	DynamicBvh tree;
	tree.Rebuild(proxies);
	for (int position = 1; position <= 32; position++) {
		proxies[0] = Cube(1, static_cast<float>(position));
		const auto preflight = tree.Preflight(proxies);
		REQUIRE(preflight.EscapedLeaves <= 1);
		REQUIRE(tree.Sync(proxies, preflight));
		CHECK(tree.Stats().CachedPairs <= 1);
		CHECK(tree.Stats().RefittedLeaves == preflight.EscapedLeaves);
		CHECK(tree.Stats().ReinsertedLeaves == 0);

		std::vector<std::pair<uint64_t, uint64_t>> actual;
		tree.ForEachOverlappingPair([&actual](const Proxy &first, const Proxy &second) {
			actual.emplace_back(std::min(first.Id, second.Id), std::max(first.Id, second.Id));
			return true;
		});
		std::vector<std::pair<uint64_t, uint64_t>> expected;
		for (size_t first = 0; first < proxies.size(); first++) {
			for (size_t second = first + 1; second < proxies.size(); second++) {
				if (proxies[first].Bounds.Overlaps(proxies[second].Bounds)) {
					expected.emplace_back(proxies[first].Id, proxies[second].Id);
				}
			}
		}
		CHECK(actual == expected);
	}
}

TEST_CASE("property-style sync histories match independent grid queries", "[dynamicbvh]") {
	std::array<Proxy, 24> proxies{};
	for (size_t index = 0; index < proxies.size(); index++) {
		const float x = -5.0f + static_cast<float>(index % 8) * 1.25f;
		const float y = static_cast<float>((index * 5) % 7) * 0.2f - 0.6f;
		const float z = static_cast<float>((index * 11) % 5) * 0.2f - 0.4f;
		proxies[index] = Proxy{
			100 + index,
			AABB::FromCentre(Vector3{x, y, z}, Vector3{0.4f, 0.4f, 0.4f}),
			LayerMask::Only(static_cast<uint32_t>(index % 3)),
		};
	}
	DynamicBvh synced;
	synced.Rebuild(proxies);
	for (size_t cycle = 0; cycle < 12; cycle++) {
		for (size_t changed = 0; changed < 1 + cycle % 4; changed++) {
			Proxy &proxy = proxies[(cycle * 7 + changed * 5) % proxies.size()];
			const Vector3 delta{
				changed % 2 == 0 ? 0.6f : -0.35f,
				static_cast<float>(cycle % 3) * 0.1f,
				static_cast<float>((cycle + changed) % 2) * 0.15f,
			};
			proxy.Bounds.Minimum = proxy.Bounds.Minimum + delta;
			proxy.Bounds.Maximum = proxy.Bounds.Maximum + delta;
		}
		REQUIRE(synced.Sync(proxies, synced.Preflight(proxies)));
		HashGrid reference;
		reference.Rebuild(proxies);

		const LayerMask mask = LayerMask::Only(static_cast<uint32_t>(cycle % 3));
		const AABB box = AABB::FromCentre(Vector3{0.0f, 0.0f, 0.0f}, Vector3{5.5f, 1.5f, 1.5f});
		std::array<uint64_t, 32> syncedIds{};
		std::array<uint64_t, 32> rebuiltIds{};
		auto syncedResult = OverlapBox(synced, box, mask, syncedIds);
		auto referenceResult = OverlapBox(reference, box, mask, rebuiltIds);
		CHECK(syncedResult.Written == referenceResult.Written);
		CHECK(syncedResult.Overflowed == referenceResult.Overflowed);
		std::sort(syncedIds.begin(), syncedIds.begin() + syncedResult.Written);
		std::sort(rebuiltIds.begin(), rebuiltIds.begin() + referenceResult.Written);
		CHECK(std::equal(syncedIds.begin(), syncedIds.begin() + syncedResult.Written, rebuiltIds.begin()));

		syncedResult = OverlapSphere(synced, Vector3{0.0f, 0.0f, 0.0f}, 4.5f, mask, syncedIds);
		referenceResult = OverlapSphere(reference, Vector3{0.0f, 0.0f, 0.0f}, 4.5f, mask, rebuiltIds);
		CHECK(syncedResult.Written == referenceResult.Written);
		CHECK(syncedResult.Overflowed == referenceResult.Overflowed);
		std::sort(syncedIds.begin(), syncedIds.begin() + syncedResult.Written);
		std::sort(rebuiltIds.begin(), rebuiltIds.begin() + referenceResult.Written);
		CHECK(std::equal(syncedIds.begin(), syncedIds.begin() + syncedResult.Written, rebuiltIds.begin()));

		const Ray ray{Vector3{-8.0f, 0.0f, 0.0f}, Vector3::XAxis};
		const auto syncedHit = Raycast(synced, ray, 20.0f, mask);
		const auto referenceHit = Raycast(reference, ray, 20.0f, mask);
		REQUIRE(syncedHit.has_value() == referenceHit.has_value());
		if (syncedHit) {
			CHECK(syncedHit->Id == referenceHit->Id);
			CHECK(syncedHit->Distance == referenceHit->Distance);
		}
		std::array<engine::core::RayHit, 32> syncedHits{};
		std::array<engine::core::RayHit, 32> rebuiltHits{};
		syncedResult = RaycastAll(synced, ray, 20.0f, mask, syncedHits);
		referenceResult = RaycastAll(reference, ray, 20.0f, mask, rebuiltHits);
		CHECK(syncedResult.Written == referenceResult.Written);
		CHECK(syncedResult.Overflowed == referenceResult.Overflowed);
		for (size_t index = 0; index < syncedResult.Written; index++) {
			CHECK(syncedHits[index].Id == rebuiltHits[index].Id);
			CHECK(syncedHits[index].Distance == rebuiltHits[index].Distance);
		}

		syncedResult = ShapeCast(
			synced,
			AABB::FromCentre(Vector3{-6.0f, 0.0f, 0.0f}, Vector3{0.2f, 0.2f, 0.2f}),
			Vector3{12.0f, 0.0f, 0.0f},
			mask,
			syncedIds
		);
		referenceResult = ShapeCast(
			reference,
			AABB::FromCentre(Vector3{-6.0f, 0.0f, 0.0f}, Vector3{0.2f, 0.2f, 0.2f}),
			Vector3{12.0f, 0.0f, 0.0f},
			mask,
			rebuiltIds
		);
		CHECK(syncedResult.Written == referenceResult.Written);
		CHECK(syncedResult.Overflowed == referenceResult.Overflowed);
		std::sort(syncedIds.begin(), syncedIds.begin() + syncedResult.Written);
		std::sort(rebuiltIds.begin(), rebuiltIds.begin() + referenceResult.Written);
		CHECK(std::equal(syncedIds.begin(), syncedIds.begin() + syncedResult.Written, rebuiltIds.begin()));
	}
}
