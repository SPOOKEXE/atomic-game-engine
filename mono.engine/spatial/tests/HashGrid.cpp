#include <engine/core/types/AABB.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Suite.hpp>

// Private, and deliberately so. The walk, the retained capacity and the bucket
// a cell lands in are all things this suite has to see and no other module
// should — `AGENTS.md` at the root: link the module's `src/` rather than
// widening its public header to make a test easier.
#include "GridInternals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.spatial.hashgrid")
// The grid is built out of boxes and every candidate is re-tested against one.
TEST_DEPENDS("engine.core.types.aabb")
// Filtering happens inside the walk, so a change to the mask changes what a
// walk returns.
TEST_DEPENDS("engine.spatial.layermask")

using engine::core::AABB;
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
}

TEST_CASE("a cell coordinate floors rather than truncating", "[hashgrid]") {
	// A truncating cast rounds toward zero, so -0.5 and +0.5 land in the same
	// cell and the cell at the origin is twice the width of every other one.
	// Nothing misses, because the build and the query would agree — which is
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
	// consumer downstream — a contact list, an overlap span — receives it four
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

TEST_CASE("a proxy too large for cells is found through the oversized list", "[hashgrid]") {
	// A baseplate. Past the cap it stops producing entries and every query
	// tests it directly instead — the path that keeps one enormous object from
	// costing tens of thousands of entries per rebuild.
	HashGrid grid{UNIT_CELL};
	const Proxy proxies[] = {
		Box(4, Vector3{-50.0f, -1.0f, -50.0f}, Vector3{50.0f, 0.0f, 50.0f}),
		Box(5, Vector3{0.2f, 0.2f, 0.2f}, Vector3{0.8f, 0.8f, 0.8f}),
	};
	grid.Rebuild(proxies);

	REQUIRE(GridInternals::OversizedCount(grid) == 1);

	// Found exactly once, from a query nowhere near where its cells would have
	// started.
	const std::vector<uint64_t> onTop =
		Visited(grid, AABB{Vector3{20.0f, -0.5f, 20.0f}, Vector3{20.5f, -0.4f, 20.5f}});
	REQUIRE(onTop == std::vector<uint64_t>{4});

	// And not found where it is not, which is what says the exact box test
	// still runs on the oversized path.
	const std::vector<uint64_t> above =
		Visited(grid, AABB{Vector3{20.0f, 5.0f, 20.0f}, Vector3{20.5f, 5.5f, 20.5f}});
	REQUIRE(above.empty());

	// The small proxy still comes out of the cells, and only once.
	const std::vector<uint64_t> both =
		Visited(grid, AABB{Vector3{0.0f, -0.5f, 0.0f}, Vector3{1.0f, 1.0f, 1.0f}});
	REQUIRE(CountOf(both, 4) == 1);
	REQUIRE(CountOf(both, 5) == 1);
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
	// says "worth testing" and the box test says "yes" — deleting the second
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

	// Capacity survives, and so does the address — the second is the stronger
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
	// it takes the pass instead. The answer must not change — only the route.
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
	// of thumb `DEFAULT_CELL_SIZE` records — and is the answer a person would
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
		// every query — which is the right default and the wrong fixture.
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
