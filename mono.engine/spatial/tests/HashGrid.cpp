#include <engine/core/Metrics.hpp>
#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

// Private, and deliberately so. The walk, the retained capacity and the bucket
// a cell lands in are all things this suite has to see and no other module
// should - `AGENTS.md` at the root: link the module's `src/` rather than
// widening its public header to make a test easier.
#include "GridInternals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.spatial.hashgrid")
// The grid is built out of boxes and every candidate is re-tested against one.
TEST_DEPENDS("engine.core.types.aabb")
// Filtering happens inside the walk, so a change to the mask changes what a
// walk returns.
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::AABB;
using engine::core::Metrics;
using engine::core::Random;
using engine::core::Vector3;
using engine::spatial::CellCoordinateOf;
using engine::spatial::GridInternals;
using engine::spatial::HashGrid;
using engine::spatial::LayerMask;
using engine::spatial::Proxy;
using engine::spatial::SuggestCellSize;
namespace core = engine::core;

namespace {
	// One metre cells, so a cell coordinate and a world coordinate are the same
	// number and every case below reads without arithmetic.
	constexpr float UNIT_CELL = 1.0f;

	Proxy
	Box(uint64_t id, const Vector3 &minimum, const Vector3 &maximum, LayerMask layers = LayerMask::All()) {
		return Proxy{id, AABB{minimum, maximum}, layers};
	}

	std::vector<uint64_t>
	Visited(const HashGrid &grid, const AABB &volume, LayerMask mask = LayerMask::All()) {
		std::vector<uint64_t> found;
		GridInternals::ForEachCandidate(grid, volume, mask, [&](const Proxy &proxy) {
			found.push_back(proxy.Id);
			return true;
		});
		return found;
	}

	size_t CountOf(const std::vector<uint64_t> &found, uint64_t id) {
		return static_cast<size_t>(std::count(found.begin(), found.end(), id));
	}

	std::vector<uint64_t> BruteOverlap(std::span<const Proxy> proxies, const AABB &volume, LayerMask mask) {
		std::vector<uint64_t> found;
		for (const Proxy &proxy : proxies) {
			if (proxy.Layers.Overlaps(mask) && proxy.Bounds.Overlaps(volume)) {
				found.push_back(proxy.Id);
			}
		}
		std::sort(found.begin(), found.end());
		return found;
	}

	void CheckOverlapOracle(
		const HashGrid &grid, std::span<const Proxy> proxies, const AABB &volume, LayerMask mask
	) {
		std::vector<uint64_t> found = Visited(grid, volume, mask);
		std::sort(found.begin(), found.end());
		REQUIRE(std::adjacent_find(found.begin(), found.end()) == found.end());
		REQUIRE(found == BruteOverlap(proxies, volume, mask));
	}

	struct DispatchSchedule {
		bool Reverse = false;
	};

	void
	DispatchRanges(void *context, size_t count, HashGrid::RangeDispatcher::Body body, void *bodyContext) {
		const auto &schedule = *static_cast<const DispatchSchedule *>(context);
		if (schedule.Reverse) {
			for (size_t index = count; index > 0; index--) {
				body(bodyContext, index - 1, index);
			}
			return;
		}

		// Odds before evens is a fixed permutation, not a merely reversed loop.
		for (size_t index = 1; index < count; index += 2) {
			body(bodyContext, index, index + 1);
		}
		for (size_t index = 0; index < count; index += 2) {
			body(bodyContext, index, index + 1);
		}
	}

	HashGrid::RangeDispatcher Dispatcher(DispatchSchedule &schedule) {
		return HashGrid::RangeDispatcher{&schedule, &DispatchRanges};
	}

	void CheckExactLayout(const HashGrid &serial, const HashGrid &parallel) {
		const std::span<const uint32_t> serialBuckets = GridInternals::BucketStarts(serial);
		const std::span<const uint32_t> parallelBuckets = GridInternals::BucketStarts(parallel);
		REQUIRE(serialBuckets.size() == parallelBuckets.size());
		REQUIRE(std::equal(serialBuckets.begin(), serialBuckets.end(), parallelBuckets.begin()));
		const std::span<const std::byte> serialEntries = GridInternals::EntryBytes(serial);
		const std::span<const std::byte> parallelEntries = GridInternals::EntryBytes(parallel);
		REQUIRE(serialEntries.size() == parallelEntries.size());
		REQUIRE(std::equal(serialEntries.begin(), serialEntries.end(), parallelEntries.begin()));
	}
}

TEST_CASE("a cell coordinate floors rather than truncating", "[hashgrid]") {
	// A truncating cast rounds toward zero, so -0.5 and +0.5 land in the same
	// cell and the cell at the origin is twice the width of every other one.
	// Nothing misses, because the build and the query would agree - which is
	// exactly why it survives: it is a silent doubling of the busiest cell in
	// every scene, since scenes are built around the origin.
	REQUIRE(CellCoordinateOf(0.5f, 1.0f) == 0);
	REQUIRE(CellCoordinateOf(-0.5f, 1.0f) == -1);
	REQUIRE(CellCoordinateOf(-1.0f, 1.0f) == -1);
	REQUIRE(CellCoordinateOf(-1.5f, 1.0f) == -2);
	REQUIRE(CellCoordinateOf(0.0f, 1.0f) == 0);

	// And with a spacing that is not one, so the multiply is exercised too.
	REQUIRE(CellCoordinateOf(-4.0f, 1.0f / 8.0f) == -1);
	REQUIRE(CellCoordinateOf(-8.0f, 1.0f / 8.0f) == -1);
	REQUIRE(CellCoordinateOf(-9.0f, 1.0f / 8.0f) == -2);
}

TEST_CASE("a rebuild observes each index phase", "[hashgrid]") {
	Metrics::Clear();

	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Box(1, Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.9f, 0.9f, 0.9f})};
	grid.Rebuild(proxies);

	// The aggregate remains the dashboard's broad rebuild number. The three
	// phases say which deterministic pass owns it when that number regresses.
	const auto rebuild = Metrics::GetHistogram("spatial.grid.rebuild");
	const auto ranges = Metrics::GetHistogram("spatial.grid.ranges");
	const auto histogram = Metrics::GetHistogram("spatial.grid.histogram");
	const auto fill = Metrics::GetHistogram("spatial.grid.fill");

	REQUIRE(rebuild.has_value());
	REQUIRE(ranges.has_value());
	REQUIRE(histogram.has_value());
	REQUIRE(fill.has_value());
	CHECK(rebuild->Samples == 1);
	CHECK(ranges->Samples == 1);
	CHECK(histogram->Samples == 1);
	CHECK(fill->Samples == 1);
	CHECK(rebuild->IsTime);
	CHECK(ranges->IsTime);
	CHECK(histogram->IsTime);
	CHECK(fill->IsTime);

	Metrics::Clear();
}

TEST_CASE("cells left of the origin are their own cells", "[hashgrid]") {
	// The same claim from the outside: a proxy entirely in negative space is
	// found there and nowhere near the mirrored positive cell.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{-2.5f, -2.5f, -2.5f}, Vector3{-2.1f, -2.1f, -2.1f}),
		Box(2, Vector3{2.1f, 2.1f, 2.1f}, Vector3{2.5f, 2.5f, 2.5f}),
	};
	grid.Rebuild(proxies);

	REQUIRE(
		Visited(grid, AABB{Vector3{-3.0f, -3.0f, -3.0f}, Vector3{-2.0f, -2.0f, -2.0f}}) ==
		std::vector<uint64_t>{1}
	);
	REQUIRE(
		Visited(grid, AABB{Vector3{2.0f, 2.0f, 2.0f}, Vector3{3.0f, 3.0f, 3.0f}}) == std::vector<uint64_t>{2}
	);
}

TEST_CASE("a proxy spanning four cells is visited once", "[hashgrid]") {
	// Without de-duplication this proxy is reported four times and every
	// consumer downstream - a contact list, an overlap span - receives it four
	// times too.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Box(7, Vector3{0.5f, 0.5f, 0.5f}, Vector3{1.5f, 1.5f, 0.5f})};
	grid.Rebuild(proxies);

	// Four entries: two cells on X, two on Y, one on Z.
	REQUIRE(GridInternals::EntryCount(grid) == 4);

	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{2.0f, 2.0f, 0.9f}});
	REQUIRE(CountOf(found, 7) == 1);
}

TEST_CASE("a query spanning four cells visits a small proxy once", "[hashgrid]") {
	// The mirror of the case above, and the one that fails when de-duplication
	// is written as "report from the query's first cell" instead of from the
	// first cell the two ranges share. The proxy sits in the query's *last*
	// cell, so that spelling reports it zero times rather than twice.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Box(9, Vector3{1.2f, 1.2f, 0.2f}, Vector3{1.8f, 1.8f, 0.8f})};
	grid.Rebuild(proxies);

	REQUIRE(GridInternals::EntryCount(grid) == 1);

	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{0.1f, 0.1f, 0.1f}, Vector3{1.9f, 1.9f, 0.9f}});
	REQUIRE(found == std::vector<uint64_t>{9});
}

TEST_CASE("a proxy whose first cell is outside the query is still visited", "[hashgrid]") {
	// The third spelling of the same mistake: "report from the proxy's first
	// cell". This proxy starts at cell zero and the query only reaches cell
	// one, so that version loses it entirely.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Box(11, Vector3{0.5f, 0.5f, 0.5f}, Vector3{1.5f, 0.5f, 0.5f})};
	grid.Rebuild(proxies);

	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{1.1f, 0.4f, 0.4f}, Vector3{1.9f, 0.6f, 0.6f}});
	REQUIRE(found == std::vector<uint64_t>{11});
}

TEST_CASE("a proxy far larger than the query volume is still found", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {Box(3, Vector3{-3.0f, -3.0f, -3.0f}, Vector3{3.0f, 3.0f, 3.0f})};
	grid.Rebuild(proxies);

	// Seven cells on each axis, which is under the cap, so it went into cells.
	REQUIRE(GridInternals::OversizedCount(grid) == 0);

	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.2f, 0.2f, 0.2f}});
	REQUIRE(found == std::vector<uint64_t>{3});
}

TEST_CASE("a large proxy promotes to a coarse level", "[hashgrid]") {
	// A baseplate belongs in one coarser grid, avoiding tens of thousands of
	// base entries while preserving the normal cell walk for small bodies.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(4, Vector3{-50.0f, -1.0f, -50.0f}, Vector3{50.0f, 0.0f, 50.0f}),
		Box(5, Vector3{0.2f, 0.2f, 0.2f}, Vector3{0.8f, 0.8f, 0.8f}),
	};
	grid.Rebuild(proxies);

	REQUIRE(GridInternals::OversizedCount(grid) == 0);
	REQUIRE(GridInternals::LevelProxyCount(grid, 2) == 1);

	// Found exactly once, from a query nowhere near where its cells would have
	// started.
	const std::vector<uint64_t> onTop =
		Visited(grid, AABB{Vector3{20.0f, -0.5f, 20.0f}, Vector3{20.5f, -0.4f, 20.5f}});
	REQUIRE(onTop == std::vector<uint64_t>{4});

	// And not found where it is not, which says the exact box test still runs.
	const std::vector<uint64_t> above =
		Visited(grid, AABB{Vector3{20.0f, 5.0f, 20.0f}, Vector3{20.5f, 5.5f, 20.5f}});
	REQUIRE(above.empty());

	// The small proxy still comes out of the cells, and only once.
	const std::vector<uint64_t> both =
		Visited(grid, AABB{Vector3{0.0f, -0.5f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}});
	REQUIRE(CountOf(both, 4) == 1);
	REQUIRE(CountOf(both, 5) == 1);
}

TEST_CASE("clamped, inverted, and nonfinite bounds stay bounded", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const float beyondBaseLimit = static_cast<float>(engine::spatial::CELL_LIMIT + 32);
	const Proxy proxies[] = {
		Box(1, Vector3{beyondBaseLimit, 0.0f, 0.0f}, Vector3{beyondBaseLimit + 0.5f, 0.5f, 0.5f}),
		Box(2, Vector3{2.0f, 0.0f, 0.0f}, Vector3{1.0f, 0.5f, 0.5f}),
		Box(3,
			Vector3{std::numeric_limits<float>::infinity(), 0.0f, 0.0f},
			Vector3{std::numeric_limits<float>::infinity(), 0.5f, 0.5f}),
	};
	grid.Rebuild(proxies);

	// `CellCoordinateOf` bounds every coordinate before the span is measured,
	// so the finite clamped box and the nonfinite pair are both bounded base
	// ranges. The inverted box cannot be represented and remains residual.
	REQUIRE(GridInternals::LevelProxyCount(grid, 0) == 2);
	REQUIRE(GridInternals::OversizedCount(grid) == 1);
	REQUIRE(
		Visited(
			grid, AABB{Vector3{beyondBaseLimit, 0.0f, 0.0f}, Vector3{beyondBaseLimit + 0.5f, 0.5f, 0.5f}}
		) == std::vector<uint64_t>{1}
	);
}

TEST_CASE("a bucket collision is a false positive and never a miss", "[hashgrid]") {
	// Two cells that hash to the same bucket. Both proxies must be findable
	// from their own cell and neither must appear in the other's answer.
	HashGrid grid{UNIT_CELL};
	const Proxy seed[] = {Box(1, Vector3{0.2f, 0.2f, 0.2f}, Vector3{0.8f, 0.8f, 0.8f})};
	grid.Rebuild(seed);

	const size_t target = GridInternals::BucketOf(grid, 0, 0, 0);
	int32_t collidingX = 0;
	for (int32_t candidate = 1; candidate < 100000; candidate++) {
		if (GridInternals::BucketOf(grid, candidate, 0, 0) == target) {
			collidingX = candidate;
			break;
		}
	}
	REQUIRE(collidingX != 0);

	const float offset = static_cast<float>(collidingX);
	const Proxy proxies[] = {
		Box(1, Vector3{0.2f, 0.2f, 0.2f}, Vector3{0.8f, 0.8f, 0.8f}),
		Box(2, Vector3{offset + 0.2f, 0.2f, 0.2f}, Vector3{offset + 0.8f, 0.8f, 0.8f}),
	};
	grid.Rebuild(proxies);

	REQUIRE(GridInternals::BucketOf(grid, 0, 0, 0) == GridInternals::BucketOf(grid, collidingX, 0, 0));

	REQUIRE(
		Visited(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}}) == std::vector<uint64_t>{1}
	);
	REQUIRE(
		Visited(grid, AABB{Vector3{offset, 0.0f, 0.0f}, Vector3{offset + 1.0f, 1.0f, 1.0f}}) ==
		std::vector<uint64_t>{2}
	);
}

TEST_CASE("sharing a cell is not overlapping, and the box test says so", "[hashgrid]") {
	// A cell is coarse: two proxies in one cell need not touch. The cell match
	// says "worth testing" and the box test says "yes" - deleting the second
	// because the first already narrowed things down turns every cell into a
	// hit, and a broad phase that reports its own cell size as a contact.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{0.05f, 0.05f, 0.05f}, Vector3{0.15f, 0.15f, 0.15f}),
		Box(2, Vector3{0.85f, 0.85f, 0.85f}, Vector3{0.95f, 0.95f, 0.95f}),
	};
	grid.Rebuild(proxies);

	// Both live in cell (0, 0, 0), so both come out of the same bucket entry
	// range and only the box test can tell them apart.
	REQUIRE(GridInternals::EntryCount(grid) == 2);
	REQUIRE(
		Visited(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.2f, 0.2f, 0.2f}}) == std::vector<uint64_t>{1}
	);
	REQUIRE(
		Visited(grid, AABB{Vector3{0.8f, 0.8f, 0.8f}, Vector3{1.0f, 1.0f, 1.0f}}) == std::vector<uint64_t>{2}
	);
}

TEST_CASE("a candidate whose layers miss the mask is not reported", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.4f, 0.4f, 0.4f}, LayerMask::Only(0)),
		Box(2, Vector3{0.5f, 0.5f, 0.5f}, Vector3{0.9f, 0.9f, 0.9f}, LayerMask::Only(1)),
		Box(3, Vector3{0.1f, 0.5f, 0.1f}, Vector3{0.4f, 0.9f, 0.4f}, LayerMask::None()),
	};
	grid.Rebuild(proxies);

	const AABB everything{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}};

	REQUIRE(Visited(grid, everything, LayerMask::Only(0)) == std::vector<uint64_t>{1});
	REQUIRE(Visited(grid, everything, LayerMask::Only(1)) == std::vector<uint64_t>{2});
	REQUIRE(
		Visited(grid, everything, LayerMask::Only(0) | LayerMask::Only(1)) == std::vector<uint64_t>{1, 2}
	);

	// On no layers at all, a proxy is in the index and invisible to every
	// query, including one asking for everything. That is what an empty mask
	// means and it has to stay unambiguous.
	REQUIRE(CountOf(Visited(grid, everything, LayerMask::All()), 3) == 0);
}

TEST_CASE("two rebuilds of the same proxies visit in the same order", "[hashgrid]") {
	// A broad phase that visits in a different order produces a different
	// solver result, and a recorded run stops replaying. Nothing about the
	// build may depend on history.
	HashGrid grid{UNIT_CELL};

	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 60; index++) {
		const float base = static_cast<float>(index % 7) + 0.1f;
		const float height = static_cast<float>(index % 5) + 0.1f;
		proxies.push_back(
			Box(index + 1, Vector3{base, height, 0.1f}, Vector3{base + 1.4f, height + 0.6f, 0.9f})
		);
	}

	const AABB everything{Vector3{0.0f, 0.0f, 0.0f}, Vector3{9.0f, 7.0f, 1.0f}};

	grid.Rebuild(proxies);
	const std::vector<uint64_t> first = Visited(grid, everything);

	grid.Rebuild(proxies);
	const std::vector<uint64_t> second = Visited(grid, everything);

	REQUIRE(first == second);
	REQUIRE(first.size() == proxies.size());

	// A fresh grid built the same way agrees too, so the order is a function of
	// the input and not of what this object did previously.
	HashGrid other{UNIT_CELL};
	other.Rebuild(proxies);
	REQUIRE(Visited(other, everything) == first);
}

TEST_CASE("a generated rebuild writes into owned proxy storage", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	grid.RebuildGenerated(
		2,
		[](std::span<Proxy> proxies) {
			proxies[0] = Box(7, Vector3{0.0f, 0.0f, 0.0f}, Vector3{3.0f, 3.0f, 3.0f});
			proxies[1] = Box(9, Vector3{8.0f, 0.0f, 0.0f}, Vector3{11.0f, 3.0f, 3.0f});
		},
		true
	);

	REQUIRE(grid.ProxyCount() == 2);
	REQUIRE(grid.CellSize() == 8.0f);
	REQUIRE(
		Visited(grid, AABB{Vector3{-1.0f, -1.0f, -1.0f}, Vector3{4.0f, 4.0f, 4.0f}}) ==
		std::vector<uint64_t>{7}
	);
}

TEST_CASE("parallel rebuild preserves exact serial cells under permuted dispatch", "[hashgrid][parallel]") {
	std::vector<Proxy> proxies;
	proxies.reserve(HashGrid::PARALLEL_MINIMUM_PROXIES);
	for (uint64_t index = 0; index < HashGrid::PARALLEL_MINIMUM_PROXIES; index++) {
		const float x = static_cast<float>(index % 128) * 2.0f;
		const float y = static_cast<float>((index / 128) % 8) * 3.0f;
		const float z = static_cast<float>(index / 1024) * 2.0f;
		proxies.push_back(Box(index + 1, Vector3{x, y, z}, Vector3{x + 1.5f, y + 1.5f, z + 1.5f}));
	}

	HashGrid serial{UNIT_CELL};
	serial.Rebuild(proxies);
	DispatchSchedule permuted;
	HashGrid parallel{UNIT_CELL};
	Metrics::Clear();
	parallel.RebuildParallel(proxies, Dispatcher(permuted));
	CheckExactLayout(serial, parallel);
	const auto parallelUsed = Metrics::GetGauge("spatial.grid.parallel.used");
	REQUIRE(parallelUsed.has_value());
	REQUIRE(parallelUsed->Value == 1.0);

	DispatchSchedule reverse{true};
	parallel.RebuildParallel(proxies, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	const size_t warmScratch = GridInternals::ParallelScratchCapacity(parallel);
	REQUIRE(warmScratch != 0);
	REQUIRE(warmScratch <= (size_t{8} << 20));
	parallel.RebuildParallel(proxies, Dispatcher(permuted));
	REQUIRE(GridInternals::ParallelScratchCapacity(parallel) == warmScratch);

	for (uint32_t index = 0; index < 64; index++) {
		const Vector3 centre{
			Random::Range(index, 3, -8.0f, 264.0f),
			Random::Range(index, 5, -4.0f, 28.0f),
			Random::Range(index, 7, -4.0f, 20.0f),
		};
		const float extent = Random::Range(index, 11, 0.2f, 8.0f);
		const AABB query = AABB::FromCentre(centre, Vector3{extent, extent, extent});
		REQUIRE(Visited(serial, query) == Visited(parallel, query));
		CheckOverlapOracle(parallel, proxies, query, LayerMask::All());
	}
}

TEST_CASE("parallel radix preserves uneven shard boundaries", "[hashgrid][parallel]") {
	std::vector<Proxy> proxies;
	proxies.reserve(HashGrid::PARALLEL_MINIMUM_PROXIES);
	for (uint64_t index = 0; index < HashGrid::PARALLEL_MINIMUM_PROXIES; index++) {
		const float x = static_cast<float>(index) * 4.0f;
		if (index < HashGrid::PARALLEL_MINIMUM_PROXIES / 2) {
			proxies.push_back(Box(index, Vector3{x, 0.0f, 0.0f}, Vector3{x + 1.5f, 1.5f, 1.5f}));
		} else {
			proxies.push_back(Box(index, Vector3{x, 0.0f, 0.0f}, Vector3{x + 0.5f, 0.5f, 0.5f}));
		}
	}

	HashGrid serial{UNIT_CELL};
	serial.Rebuild(proxies);
	DispatchSchedule reverse{true};
	HashGrid parallel{UNIT_CELL};
	Metrics::Clear();
	parallel.RebuildParallel(proxies, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	const auto parallelUsed = Metrics::GetGauge("spatial.grid.parallel.used");
	REQUIRE(parallelUsed.has_value());
	REQUIRE(parallelUsed->Value == 1.0);
}

TEST_CASE(
	"parallel rebuild falls back without changing hierarchy or residual layout", "[hashgrid][parallel]"
) {
	std::vector<Proxy> proxies;
	proxies.reserve(HashGrid::PARALLEL_MINIMUM_PROXIES + 259);
	for (uint64_t index = 0; index < HashGrid::PARALLEL_MINIMUM_PROXIES; index++) {
		const float x = static_cast<float>(index % 128) * 2.0f;
		const float y = static_cast<float>((index / 128) % 8) * 2.0f;
		proxies.push_back(Box(index + 1, Vector3{x, y, 0.0f}, Vector3{x + 0.5f, y + 0.5f, 0.5f}));
	}
	for (uint64_t index = 0; index < 257; index++) {
		proxies.push_back(
			Box(HashGrid::PARALLEL_MINIMUM_PROXIES + index + 1,
				Vector3{-100.0f, -100.0f, -100.0f},
				Vector3{100.0f, 100.0f, 100.0f})
		);
	}
	proxies.push_back(
		Box(HashGrid::PARALLEL_MINIMUM_PROXIES + 300,
			Vector3{-1000000.0f, 10.0f, -1000000.0f},
			Vector3{1000000.0f, 12.0f, 1000000.0f})
	);

	HashGrid serial{UNIT_CELL};
	serial.Rebuild(proxies);
	DispatchSchedule reverse{true};
	HashGrid parallel{UNIT_CELL};
	Metrics::Clear();
	parallel.RebuildParallel(proxies, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	REQUIRE(GridInternals::OversizedCount(parallel) == GridInternals::OversizedCount(serial));
	const auto hierarchyFallback = Metrics::Get("spatial.grid.parallel.hierarchy_serial_fallbacks");
	const auto parallelUsed = Metrics::GetGauge("spatial.grid.parallel.used");
	REQUIRE(hierarchyFallback.has_value());
	REQUIRE(hierarchyFallback->Value == 1.0);
	REQUIRE(parallelUsed.has_value());
	REQUIRE(parallelUsed->Value == 0.0);

	const AABB centre{Vector3{-0.5f, -0.5f, -0.5f}, Vector3{0.5f, 0.5f, 0.5f}};
	REQUIRE(Visited(serial, centre) == Visited(parallel, centre));
	CheckOverlapOracle(parallel, proxies, centre, LayerMask::All());

	Metrics::Clear();
	parallel.RebuildParallel(proxies, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	const auto cachedFallback = Metrics::Get("spatial.grid.parallel.hierarchy_cached_serial_fallbacks");
	REQUIRE(cachedFallback.has_value());
	REQUIRE(cachedFallback->Value == 1.0);

	std::vector<Proxy> ordinary = proxies;
	ordinary.resize(HashGrid::PARALLEL_MINIMUM_PROXIES);
	serial.Rebuild(ordinary);
	Metrics::Clear();
	parallel.RebuildParallel(ordinary, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	const auto transitionFallback = Metrics::Get("spatial.grid.parallel.hierarchy_cached_serial_fallbacks");
	REQUIRE(transitionFallback.has_value());
	REQUIRE(transitionFallback->Value == 1.0);
	Metrics::Clear();
	parallel.RebuildParallel(ordinary, Dispatcher(reverse));
	CheckExactLayout(serial, parallel);
	const auto transitionParallel = Metrics::GetGauge("spatial.grid.parallel.used");
	REQUIRE(transitionParallel.has_value());
	REQUIRE(transitionParallel->Value == 1.0);
}

TEST_CASE("parallel rebuild handles empty grow and shrink without scratch growth", "[hashgrid][parallel]") {
	DispatchSchedule schedule;
	HashGrid grid{UNIT_CELL};
	grid.RebuildParallel({}, Dispatcher(schedule));
	REQUIRE(grid.ProxyCount() == 0);

	std::vector<Proxy> large;
	for (uint64_t index = 0; index < HashGrid::PARALLEL_MINIMUM_PROXIES; index++) {
		const float x = static_cast<float>(index) * 2.0f;
		large.push_back(Box(index, Vector3{x, 0.0f, 0.0f}, Vector3{x + 0.5f, 0.5f, 0.5f}));
	}
	grid.RebuildParallel(large, Dispatcher(schedule));
	const size_t scratch = GridInternals::ParallelScratchCapacity(grid);
	const auto parallelUsed = Metrics::GetGauge("spatial.grid.parallel.used");
	REQUIRE(parallelUsed.has_value());
	REQUIRE(parallelUsed->Value == 1.0);
	const std::vector<Proxy> small(8, large.front());
	grid.RebuildParallel(small, Dispatcher(schedule));
	grid.RebuildParallel(large, Dispatcher(schedule));
	REQUIRE(GridInternals::ParallelScratchCapacity(grid) == scratch);
	REQUIRE(GridInternals::ParallelScratchCapacity(grid) <= (size_t{8} << 20));
}

TEST_CASE("the second rebuild reuses the first's storage", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};

	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 200; index++) {
		const float base = static_cast<float>(index);
		proxies.push_back(Box(index + 1, Vector3{base, 0.1f, 0.1f}, Vector3{base + 0.9f, 0.9f, 0.9f}));
	}

	grid.Rebuild(proxies);
	const size_t capacity = GridInternals::EntryCapacity(grid);
	const void *storage = GridInternals::EntryData(grid);
	REQUIRE(capacity >= proxies.size());

	grid.Rebuild(proxies);

	// Capacity survives, and so does the address - the second is the stronger
	// claim, because a vector that reallocated to the same size would keep the
	// first number and not the second.
	REQUIRE(GridInternals::EntryCapacity(grid) == capacity);
	REQUIRE(GridInternals::EntryData(grid) == storage);

	// Clear frees nothing either, which is what makes a grid rebuilt every tick
	// allocate on the first tick and never again.
	grid.Clear();
	REQUIRE(grid.ProxyCount() == 0);
	REQUIRE(GridInternals::EntryCapacity(grid) == capacity);
}

TEST_CASE("exclusive hierarchy levels and the residual preserve exact candidates", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.5f, 0.5f, 0.5f}, LayerMask::Only(0)),
		Box(2, Vector3{-25.0f, 0.0f, -5.0f}, Vector3{25.0f, 0.5f, 6.0f}, LayerMask::Only(0)),
		Box(3, Vector3{-75.0f, 0.0f, -5.0f}, Vector3{75.0f, 0.5f, 6.0f}, LayerMask::Only(1)),
		Box(4, Vector3{-600.0f, 0.0f, -5.0f}, Vector3{600.0f, 0.5f, 6.0f}, LayerMask::Only(1)),
		Box(5, Vector3{-4800.0f, 0.0f, -5.0f}, Vector3{4800.0f, 0.5f, 6.0f}, LayerMask::Only(0)),
		Box(6,
			Vector3{-1000000.0f, 0.0f, -1000000.0f},
			Vector3{1000000.0f, 0.5f, 1000000.0f},
			LayerMask::Only(0)),
	};
	grid.Rebuild(proxies);

	for (size_t level = 0; level < HashGrid::HIERARCHY_LEVEL_COUNT; level++) {
		REQUIRE(GridInternals::LevelProxyCount(grid, level) == 1);
	}
	REQUIRE(GridInternals::OversizedCount(grid) == 1);

	const AABB centre{Vector3{-0.1f, -0.1f, -0.1f}, Vector3{0.1f, 0.1f, 0.1f}};
	const std::vector<uint64_t> first = Visited(grid, centre, LayerMask::All());
	REQUIRE(first == std::vector<uint64_t>{1, 2, 3, 4, 5, 6});
	REQUIRE(Visited(grid, centre, LayerMask::Only(1)) == std::vector<uint64_t>{3, 4});

	grid.Rebuild(proxies);
	REQUIRE(Visited(grid, centre, LayerMask::All()) == first);
}

TEST_CASE("equally long axes promote symmetrically", "[hashgrid]") {
	// Promotion measures all three cell spans. A long wall on Y or Z must not
	// take a different route from the equivalent wall on X.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{-300.0f, 0.0f, 0.0f}, Vector3{300.0f, 0.5f, 0.5f}),
		Box(2, Vector3{0.0f, -300.0f, 10.0f}, Vector3{0.5f, 300.0f, 10.5f}),
		Box(3, Vector3{10.0f, 0.0f, -300.0f}, Vector3{10.5f, 0.5f, 300.0f}),
	};
	grid.Rebuild(proxies);

	const size_t level = 2;
	REQUIRE(GridInternals::LevelProxyCount(grid, level) == 3);
	REQUIRE(
		Visited(grid, AABB{Vector3{250.0f, 0.0f, 0.0f}, Vector3{251.0f, 0.5f, 0.5f}}) ==
		std::vector<uint64_t>{1}
	);
	REQUIRE(
		Visited(grid, AABB{Vector3{0.0f, 250.0f, 10.0f}, Vector3{0.5f, 251.0f, 10.5f}}) ==
		std::vector<uint64_t>{2}
	);
	REQUIRE(
		Visited(grid, AABB{Vector3{10.0f, 0.0f, 250.0f}, Vector3{10.5f, 0.5f, 251.0f}}) ==
		std::vector<uint64_t>{3}
	);
}

TEST_CASE("dense coarse levels join the residual in input order", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	std::vector<Proxy> proxies;
	proxies.push_back(
		Box(1, Vector3{-1000000.0f, -1.0f, -1000000.0f}, Vector3{1000000.0f, 1.0f, 1000000.0f})
	);
	for (uint64_t index = 0; index < 257; index++) {
		proxies.push_back(
			Box(index + 2, Vector3{-100.0f, -100.0f, -100.0f}, Vector3{100.0f, 100.0f, 100.0f})
		);
	}
	proxies.push_back(
		Box(259, Vector3{-1000000.0f, 10.0f, -1000000.0f}, Vector3{1000000.0f, 12.0f, 1000000.0f})
	);
	grid.Rebuild(proxies);

	// The 257 identical promoted ranges land in one level-three bucket. That
	// level cannot cull, so all its proxies become ordered residual candidates.
	REQUIRE(GridInternals::LevelProxyCount(grid, 3) == 0);
	REQUIRE(GridInternals::OversizedCount(grid) == proxies.size());
	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{-0.5f, -0.5f, -0.5f}, Vector3{0.5f, 0.5f, 0.5f}});
	REQUIRE(found.size() == 258);
	for (uint64_t index = 0; index < found.size(); index++) {
		REQUIRE(found[index] == index + 1);
	}
}

TEST_CASE("hierarchy capacity stabilises after its first high-water rebuild", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	std::vector<Proxy> high;
	high.push_back(Box(999, Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f}));
	for (uint64_t index = 0; index < 64; index++) {
		const float offset = static_cast<float>(index) * 2048.0f;
		high.push_back(
			Box(index, Vector3{offset, 0.0f, offset}, Vector3{offset + 1200.0f, 1.0f, offset + 1200.0f})
		);
	}
	const std::vector<Proxy> low{Box(1, Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f})};

	grid.Rebuild(high);
	const size_t highWater = GridInternals::RetainedHierarchyBytes(grid);
	for (int cycle = 0; cycle < 3; cycle++) {
		grid.Rebuild(low);
		REQUIRE(GridInternals::RetainedHierarchyBytes(grid) == highWater);
		grid.Rebuild(high);
		REQUIRE(GridInternals::RetainedHierarchyBytes(grid) == highWater);
	}
}

TEST_CASE("a hierarchy to normal transition restores the base-only rebuild", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const std::vector<Proxy> hierarchy{
		Box(1, Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.9f, 0.9f, 0.9f}),
		Box(2, Vector3{-300.0f, 0.1f, 0.1f}, Vector3{300.0f, 0.9f, 0.9f}),
	};
	std::vector<Proxy> normal;
	for (uint64_t index = 0; index < 64; index++) {
		const float offset = static_cast<float>(index) * 2.0f;
		normal.push_back(Box(index + 10, Vector3{offset, 0.1f, 0.1f}, Vector3{offset + 0.9f, 0.9f, 0.9f}));
	}

	grid.Rebuild(hierarchy);
	REQUIRE(GridInternals::LevelProxyCount(grid, 2) == 1);

	grid.Rebuild(normal);
	REQUIRE(GridInternals::OversizedCount(grid) == 0);
	for (size_t level = 1; level < HashGrid::HIERARCHY_LEVEL_COUNT; level++) {
		REQUIRE(GridInternals::LevelProxyCount(grid, level) == 0);
	}
	REQUIRE(
		Visited(grid, AABB{Vector3{19.9f, 0.0f, 0.0f}, Vector3{20.1f, 1.0f, 1.0f}}) ==
		std::vector<uint64_t>{20}
	);
	const size_t capacity = GridInternals::EntryCapacity(grid);
	const void *storage = GridInternals::EntryData(grid);

	grid.Rebuild(normal);
	REQUIRE(GridInternals::EntryCapacity(grid) == capacity);
	REQUIRE(GridInternals::EntryData(grid) == storage);
	REQUIRE(
		Visited(grid, AABB{Vector3{19.9f, 0.0f, 0.0f}, Vector3{20.1f, 1.0f, 1.0f}}) ==
		std::vector<uint64_t>{20}
	);
}

TEST_CASE("hierarchy overlap agrees with a deterministic exact oracle through rebuild cycles", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	std::vector<Proxy> proxies;
	proxies.reserve(24 + 4 + 257 + 1);
	for (uint32_t index = 0; index < 24; index++) {
		Vector3 centre{
			Random::Range(index, 3, -256.0f, 256.0f),
			Random::Range(index, 5, -96.0f, 96.0f),
			Random::Range(index, 7, -256.0f, 256.0f),
		};
		if (index == 0) {
			centre = Vector3{768.0f, 0.0f, 768.0f};
		} else if (index == 1) {
			centre = Vector3{768.0f, 48.0f, 768.0f};
		}
		const float extent = Random::Range(index, 11, 0.2f, 0.9f);
		proxies.push_back(
			Box(index + 1,
				centre - Vector3{extent, extent, extent},
				centre + Vector3{extent, extent, extent},
				LayerMask::Only(index % 2))
		);
	}

	// The first, second, third, and fourth coarse levels each receive one
	// otherwise sparse proxy. The third one is later rejected with its dense group.
	proxies.push_back(
		Box(25, Vector3{-25.0f, 80.0f, -5.0f}, Vector3{25.0f, 80.5f, 6.0f}, LayerMask::Only(0))
	);
	proxies.push_back(
		Box(26, Vector3{-75.0f, -40.0f, -5.0f}, Vector3{75.0f, -39.5f, 6.0f}, LayerMask::Only(1))
	);
	proxies.push_back(
		Box(27, Vector3{-600.0f, 40.0f, -5.0f}, Vector3{600.0f, 40.5f, 6.0f}, LayerMask::Only(0))
	);
	proxies.push_back(
		Box(28, Vector3{-4800.0f, -80.0f, -5.0f}, Vector3{4800.0f, -79.5f, 6.0f}, LayerMask::Only(1))
	);
	for (uint32_t index = 0; index < 257; index++) {
		proxies.push_back(
			Box(index + 29,
				Vector3{-100.0f, -100.0f, -100.0f},
				Vector3{100.0f, 100.0f, 100.0f},
				LayerMask::Only(index % 2))
		);
	}
	proxies.push_back(
		Box(286,
			Vector3{-1000000.0f, -1000.0f, -1000000.0f},
			Vector3{1000000.0f, 1000.0f, 1000000.0f},
			LayerMask::Only(0))
	);
	const std::vector<Proxy> base(proxies.begin(), proxies.begin() + 24);
	const std::array<LayerMask, 3> masks{LayerMask::All(), LayerMask::Only(0), LayerMask::Only(1)};

	auto check = [&](std::span<const Proxy> scene, uint32_t salt) {
		for (uint32_t index = 0; index < 32; index++) {
			const Vector3 centre{
				Random::Range(index + salt, 13, -512.0f, 512.0f),
				Random::Range(index + salt, 17, -128.0f, 128.0f),
				Random::Range(index + salt, 19, -512.0f, 512.0f),
			};
			const float extent = Random::Range(index + salt, 23, 1.0f, 24.0f);
			CheckOverlapOracle(
				grid,
				scene,
				AABB::FromCentre(centre, Vector3{extent, extent, extent}),
				masks[index % masks.size()]
			);
		}
	};

	grid.Rebuild(proxies);
	REQUIRE(GridInternals::LevelProxyCount(grid, 1) == 1);
	REQUIRE(GridInternals::LevelProxyCount(grid, 2) == 1);
	REQUIRE(GridInternals::LevelProxyCount(grid, 3) == 0);
	REQUIRE(GridInternals::LevelProxyCount(grid, 4) == 1);
	REQUIRE(GridInternals::OversizedCount(grid) == 259);
	const size_t highWater = GridInternals::RetainedHierarchyBytes(grid);
	// IDs one and two differ only on Y. The elevated query must not lose ID two
	// by treating the XZ placement as a complete three-dimensional cell key.
	REQUIRE(
		Visited(
			grid, AABB{Vector3{767.0f, 47.0f, 767.0f}, Vector3{769.0f, 49.0f, 769.0f}}, LayerMask::Only(1)
		) == std::vector<uint64_t>{2}
	);
	check(proxies, 0);

	for (uint32_t cycle = 0; cycle < 3; cycle++) {
		grid.Rebuild(base);
		check(base, 100 + cycle * 32);
		grid.Rebuild(proxies);
		REQUIRE(GridInternals::RetainedHierarchyBytes(grid) == highWater);
		check(proxies, 200 + cycle * 32);
	}
}

TEST_CASE("an empty grid answers every query with nothing", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};

	REQUIRE(grid.ProxyCount() == 0);
	REQUIRE(Visited(grid, AABB{Vector3{-5.0f, -5.0f, -5.0f}, Vector3{5.0f, 5.0f, 5.0f}}).empty());

	const Proxy proxies[] = {Box(1, Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.9f, 0.9f, 0.9f})};
	grid.Rebuild(proxies);
	REQUIRE(grid.ProxyCount() == 1);

	grid.Clear();
	REQUIRE(Visited(grid, AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}}).empty());
}

TEST_CASE("a cell size at or below zero falls back rather than dividing by it", "[hashgrid]") {
	// One divided by zero is an infinity, and every cell coordinate after it is
	// a NaN cast to an integer. The fallback keeps the grid working and makes
	// the caller's cell-size experiment stop having any effect, which is a
	// symptom somebody investigates.
	REQUIRE(HashGrid{0.0f}.CellSize() == HashGrid::DEFAULT_CELL_SIZE);
	REQUIRE(HashGrid{-4.0f}.CellSize() == HashGrid::DEFAULT_CELL_SIZE);
	REQUIRE(HashGrid{2.0f}.CellSize() == 2.0f);
}

TEST_CASE("a query volume far larger than the index still answers exactly", "[hashgrid]") {
	// Past a point the walk would cost more than one pass over every proxy, so
	// it takes the pass instead. The answer must not change - only the route.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(1, Vector3{0.1f, 0.1f, 0.1f}, Vector3{0.9f, 0.9f, 0.9f}),
		Box(2, Vector3{500.1f, 0.1f, 0.1f}, Vector3{500.9f, 0.9f, 0.9f}),
	};
	grid.Rebuild(proxies);

	const std::vector<uint64_t> found =
		Visited(grid, AABB{Vector3{-2000.0f, -2000.0f, -2000.0f}, Vector3{2000.0f, 2000.0f, 2000.0f}});
	REQUIRE(found.size() == 2);
	REQUIRE(CountOf(found, 1) == 1);
	REQUIRE(CountOf(found, 2) == 1);
}

TEST_CASE("a visitor that stops is not called again", "[hashgrid]") {
	// What an output span that has filled up does. Walking the rest of a grid
	// with nowhere to put the answer is work with no result.
	HashGrid grid{UNIT_CELL};
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 20; index++) {
		const float base = static_cast<float>(index);
		proxies.push_back(Box(index + 1, Vector3{base + 0.1f, 0.1f, 0.1f}, Vector3{base + 0.9f, 0.9f, 0.9f}));
	}
	grid.Rebuild(proxies);

	size_t calls = 0;
	const bool completed = GridInternals::ForEachCandidate(
		grid,
		AABB{Vector3{0.0f, 0.0f, 0.0f}, Vector3{20.0f, 1.0f, 1.0f}},
		LayerMask::All(),
		[&](const Proxy &) {
			calls++;
			return calls < 3;
		}
	);

	REQUIRE_FALSE(completed);
	REQUIRE(calls == 3);
}

// --- sizing the grid from what goes in it -------------------------------------

TEST_CASE("a suggested cell size is twice the mean extent, quantised", "[spatial][hashgrid]") {
	// A scene of two-metre colliders wants four-metre cells, which is the rule
	// of thumb `DEFAULT_CELL_SIZE` records - and is the answer a person would
	// have picked.
	std::vector<Proxy> proxies;
	for (int index = 0; index < 16; index++) {
		Proxy proxy;
		proxy.Id = static_cast<uint64_t>(index);
		proxy.Bounds = core::AABB::FromCentre(
			core::Vector3{static_cast<float>(index) * 4.0f, 0.0f, 0.0f}, core::Vector3{1.0f, 1.0f, 1.0f}
		);
		proxies.push_back(proxy);
	}

	CHECK(SuggestCellSize(proxies) == 4.0f);

	// **Quantised, which is what makes it stable.** Nudging one collider must
	// not move the answer, because every move costs a full rebuild of the index.
	proxies.front().Bounds =
		core::AABB::FromCentre(core::Vector3{0.0f, 0.0f, 0.0f}, core::Vector3{1.2f, 1.0f, 1.0f});
	CHECK(SuggestCellSize(proxies) == 4.0f);

	// It takes a real change of scale to move it. Ten-metre colliders want
	// cells an order of magnitude bigger.
	for (Proxy &proxy : proxies) {
		proxy.Bounds = core::AABB::FromCentre(proxy.Bounds.Centre(), core::Vector3{5.0f, 5.0f, 5.0f});
	}
	CHECK(SuggestCellSize(proxies) > 4.0f);
}

TEST_CASE("a suggestion is bounded and survives a degenerate scene", "[spatial][hashgrid]") {
	// An empty set has nothing to measure.
	CHECK(SuggestCellSize({}) == HashGrid::DEFAULT_CELL_SIZE);

	// **A world of points would suggest nothing at all**, and a division by a
	// zero cell size reaches every later query as a NaN.
	std::vector<Proxy> degenerate(4);
	for (size_t index = 0; index < degenerate.size(); index++) {
		degenerate[index].Id = index;
		degenerate[index].Bounds = core::AABB::FromCentre(core::Vector3{}, core::Vector3{});
	}
	CHECK(SuggestCellSize(degenerate) == HashGrid::DEFAULT_CELL_SIZE);

	// A world of one enormous collider is clamped rather than believed: the
	// measurement is right about a scene that is about to gain a crate.
	std::vector<Proxy> huge(1);
	huge[0].Bounds = core::AABB::FromCentre(core::Vector3{}, core::Vector3{100000.0f, 1.0f, 1.0f});
	CHECK(SuggestCellSize(huge) <= HashGrid::MAXIMUM_CELL_SIZE);

	std::vector<Proxy> tiny(1);
	tiny[0].Bounds = core::AABB::FromCentre(core::Vector3{}, core::Vector3{0.0001f, 0.0001f, 0.0001f});
	CHECK(SuggestCellSize(tiny) >= HashGrid::MINIMUM_CELL_SIZE);
}

TEST_CASE("changing the cell size empties the grid and answers the same", "[spatial][hashgrid]") {
	std::vector<Proxy> proxies;
	for (int index = 0; index < 64; index++) {
		Proxy proxy;
		proxy.Id = static_cast<uint64_t>(index);
		proxy.Bounds = core::AABB::FromCentre(
			core::Vector3{static_cast<float>(index % 8) * 3.0f, 0.0f, static_cast<float>(index / 8) * 3.0f},
			core::Vector3{1.0f, 1.0f, 1.0f}
		);

		// A default mask matches nothing, so a proxy with one is invisible to
		// every query - which is the right default and the wrong fixture.
		proxy.Layers = LayerMask::All();
		proxies.push_back(proxy);
	}

	HashGrid grid(4.0f);
	grid.Rebuild(proxies);

	const core::AABB volume =
		core::AABB::FromCentre(core::Vector3{6.0f, 0.0f, 6.0f}, core::Vector3{5.0f, 5.0f, 5.0f});

	std::array<uint64_t, 128> found{};
	const size_t before = engine::spatial::OverlapBox(grid, volume, LayerMask::All(), found).Written;
	REQUIRE(before > 0);

	// **Emptied, because every entry records a cell coordinate derived from the
	// spacing.** A grid that kept them would answer against cells that no longer
	// exist.
	grid.SetCellSize(16.0f);
	CHECK(grid.ProxyCount() == 0);
	CHECK(grid.CellSize() == 16.0f);
	CHECK(engine::spatial::OverlapBox(grid, volume, LayerMask::All(), found).Written == 0);

	// **And the same set comes back after the rebuild the caller owes it.** A
	// cell size is speed and never behaviour: the walk is exhaustive at any
	// spacing, so a query that changed its answer would be a bug in the index
	// rather than a tuning decision.
	grid.Rebuild(proxies);
	std::array<uint64_t, 128> after{};
	const size_t coarse = engine::spatial::OverlapBox(grid, volume, LayerMask::All(), after).Written;
	CHECK(coarse == before);

	std::sort(found.begin(), found.begin() + static_cast<long>(before));
	std::sort(after.begin(), after.begin() + static_cast<long>(coarse));
	CHECK(std::equal(found.begin(), found.begin() + static_cast<long>(before), after.begin()));

	// Asking for the size it already has changes nothing, which is what makes
	// the per-tick call in `SyncBroadphase` free.
	grid.SetCellSize(16.0f);
	CHECK(grid.ProxyCount() == proxies.size());

	// A size at or below zero is refused in favour of the default.
	grid.SetCellSize(-1.0f);
	CHECK(grid.CellSize() == HashGrid::DEFAULT_CELL_SIZE);
}

TEST_CASE("grid stats distinguish live rows from retained capacity", "[hashgrid]") {
	HashGrid grid{UNIT_CELL};
	const std::vector<Proxy> proxies{Box(1, Vector3::Zero, Vector3{0.5f, 0.5f, 0.5f})};
	grid.Rebuild(proxies);
	const engine::spatial::HashGridStats warm = grid.Stats();
	REQUIRE(warm.LiveBytes > 0);
	REQUIRE(warm.RetainedBytes >= warm.LiveBytes);

	grid.Clear();
	const engine::spatial::HashGridStats cleared = grid.Stats();
	CHECK(cleared.LiveBytes == 0);
	CHECK(cleared.RetainedBytes == warm.RetainedBytes);
}
