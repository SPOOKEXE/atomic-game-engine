#include <engine/core/types/AABB.hpp>
#include <engine/spatial/ChunkMap.hpp>
#include <engine/testing/Suite.hpp>

// Private, for `tests/HashGrid.cpp`'s reason: the retained capacity is a claim
// about the allocation table and not a fact a public header should carry.
#include "ChunkInternals.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

TEST_SUITE_ID("engine.spatial.chunkmap")
// The partition bins the centre of a box, so a change to how a box reports its
// bounds changes where everything lands.
TEST_DEPENDS("engine.core.types.aabb")

using engine::core::AABB;
using engine::core::Vector3;
using engine::spatial::ChunkCoordinate;
using engine::spatial::ChunkInternals;
using engine::spatial::ChunkMap;
using engine::spatial::LayerMask;
using engine::spatial::Proxy;
using engine::spatial::SuggestChunkSize;

namespace {
	// One metre chunks, so a chunk coordinate and a world coordinate are the
	// same number and every case below reads without arithmetic.
	constexpr float UNIT_CHUNK = 1.0f;

	// A proxy that is a point, so its centre is exactly where it is put. Most
	// cases here are about which chunk a centre lands in and a width would only
	// be a second thing to get right.
	Proxy At(uint64_t id, float x, float y, float z) {
		return Proxy{id, AABB{Vector3{x, y, z}, Vector3{x, y, z}}, LayerMask::All()};
	}

	Proxy Box(uint64_t id, const Vector3 &minimum, const Vector3 &maximum) {
		return Proxy{id, AABB{minimum, maximum}, LayerMask::All()};
	}

	// Every proxy index the map holds, chunk by chunk, in walk order.
	std::vector<uint32_t> WalkOrder(const ChunkMap &map) {
		std::vector<uint32_t> order;
		for (size_t chunk = 0; chunk < map.ChunkCount(); chunk++) {
			for (uint32_t member : map.MembersOf(chunk)) {
				order.push_back(member);
			}
		}
		return order;
	}
}

TEST_CASE("every proxy lands in exactly one chunk", "[chunkmap]") {
	// The property the whole structure exists for. A grid reports a box from
	// every cell it spans; a partition may not, because a caller running one
	// chunk per thread would then hand the same proxy to two of them.
	const std::vector<Proxy> proxies{
		At(0, 0.5f, 0.5f, 0.5f),
		Box(1, Vector3{-4.0f, -4.0f, -4.0f}, Vector3{4.0f, 4.0f, 4.0f}),
		At(2, 7.5f, 0.5f, 0.5f),
	};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);

	std::vector<uint32_t> order = WalkOrder(map);
	std::sort(order.begin(), order.end());
	CHECK(order == std::vector<uint32_t>{0, 1, 2});

	// Including the box nine chunks wide, which a grid would have reported from
	// every one of them. Its centre is the origin, so it is in that one chunk.
	CHECK(map.ChunkOfProxy(1) == map.ChunkOfProxy(0));
}

TEST_CASE("a chunk coordinate floors rather than truncating", "[chunkmap]") {
	// `GridInternals::CellCoordinateOf`'s case, restated here because this is a
	// second caller of it and the failure is the same one: a cast toward zero
	// puts -0.5 and +0.5 in one chunk, which makes the chunk at the origin twice
	// the width of every other. Scenes are built around the origin, so nothing
	// written with positive coordinates ever notices.
	const std::vector<Proxy> proxies{At(0, -0.5f, 0.5f, 0.5f), At(1, 0.5f, 0.5f, 0.5f)};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);

	REQUIRE(map.ChunkCount() == 2);
	CHECK(map.ChunkOfProxy(0) != map.ChunkOfProxy(1));
	CHECK(map.CoordinateAt(map.ChunkOfProxy(0)) == ChunkCoordinate{-1, 0, 0});
	CHECK(map.CoordinateAt(map.ChunkOfProxy(1)) == ChunkCoordinate{0, 0, 0});
}

TEST_CASE("members come out ascending inside a chunk", "[chunkmap]") {
	// What `MembersOf` promises, and what lets a consumer merge a chunk's
	// members against a second list ordered the same way rather than searching
	// it. The proxies are given in an order that is not the sorted one, so a
	// build that carried the input order through would fail this.
	const std::vector<Proxy> proxies{
		At(0, 0.25f, 0.25f, 0.25f),
		At(1, 5.5f, 0.5f, 0.5f),
		At(2, 0.75f, 0.75f, 0.75f),
		At(3, 0.10f, 0.10f, 0.10f),
	};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);

	const uint32_t home = map.ChunkOfProxy(0);
	const std::span<const uint32_t> members = map.MembersOf(home);
	REQUIRE(members.size() == 3);
	CHECK(std::is_sorted(members.begin(), members.end()));
	CHECK(members[0] == 0);
	CHECK(members[1] == 2);
	CHECK(members[2] == 3);
}

TEST_CASE("chunks come out ascending by coordinate", "[chunkmap]") {
	// The order a consumer splits work by, and it has to be a function of the
	// contents alone - see `ChunkCoordinate::operator<`. Built back to front so
	// that a map carrying the input order through would report them back to
	// front.
	const std::vector<Proxy> proxies{At(0, 2.5f, 0.5f, 0.5f), At(1, 0.5f, 0.5f, 0.5f)};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);

	REQUIRE(map.ChunkCount() == 2);
	CHECK(map.CoordinateAt(0) == ChunkCoordinate{0, 0, 0});
	CHECK(map.CoordinateAt(1) == ChunkCoordinate{2, 0, 0});
}

TEST_CASE("two rebuilds of one input walk identically", "[chunkmap]") {
	// The determinism `AGENTS.md` requires of every build in this module. The
	// second rebuild runs against storage the first one filled, so a build that
	// depended on what was already there rather than on the input would differ
	// here and nowhere else.
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 200; index++) {
		const auto step = static_cast<float>(index);
		proxies.push_back(At(index, step * 0.37f, step * -0.11f, step * 0.53f));
	}

	ChunkMap map(4.0f);
	map.Rebuild(proxies);
	const std::vector<uint32_t> first = WalkOrder(map);
	const size_t chunks = map.ChunkCount();

	map.Rebuild(proxies);
	CHECK(map.ChunkCount() == chunks);
	CHECK(WalkOrder(map) == first);

	// And a map built fresh agrees with one rebuilt, which is the half that
	// catches state surviving a `Clear`.
	ChunkMap fresh(4.0f);
	fresh.Rebuild(proxies);
	CHECK(WalkOrder(fresh) == first);
}

TEST_CASE("a rebuild over a steady scene allocates once", "[chunkmap]") {
	// The allocation table's standing rule, and this structure is rebuilt every
	// tick by the solver. Cleared and never freed, exactly as `HashGrid` is.
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 500; index++) {
		const auto step = static_cast<float>(index);
		proxies.push_back(At(index, step * 1.7f, 0.5f, step * 0.9f));
	}

	ChunkMap map(8.0f);
	map.Rebuild(proxies);

	const size_t members = ChunkInternals::MemberCapacity(map);
	const size_t placements = ChunkInternals::PlacementCapacity(map);
	const void *data = ChunkInternals::MemberData(map);

	map.Rebuild(proxies);

	CHECK(ChunkInternals::MemberCapacity(map) == members);
	CHECK(ChunkInternals::PlacementCapacity(map) == placements);
	CHECK(ChunkInternals::MemberData(map) == data);
}

TEST_CASE("an empty rebuild leaves a map nothing can subscript past", "[chunkmap]") {
	// `MembersOf` reads `Starts[chunk + 1]`, so the offsets array is one longer
	// than the chunk list on every path - including the one that returns early.
	ChunkMap map(UNIT_CHUNK);
	map.Rebuild({});
	CHECK(map.ChunkCount() == 0);
	CHECK(map.ProxyCount() == 0);

	// And a rebuild after an empty one still works, which is the case where the
	// early return left the offsets in a state the filling pass then appended to.
	const std::vector<Proxy> proxies{At(0, 0.5f, 0.5f, 0.5f)};
	map.Rebuild(proxies);
	REQUIRE(map.ChunkCount() == 1);
	CHECK(map.MembersOf(0).size() == 1);
}

TEST_CASE("a size change empties the map", "[chunkmap]") {
	// Every membership is a function of the spacing, so a map that kept them
	// across a change would answer against chunks that no longer exist.
	const std::vector<Proxy> proxies{At(0, 0.5f, 0.5f, 0.5f), At(1, 2.5f, 0.5f, 0.5f)};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);
	REQUIRE(map.ChunkCount() == 2);

	map.SetChunkSize(8.0f);
	CHECK(map.ChunkCount() == 0);
	CHECK(map.ChunkSize() == 8.0f);

	map.Rebuild(proxies);
	CHECK(map.ChunkCount() == 1);
}

TEST_CASE("a size change to the size it already has drops nothing", "[chunkmap]") {
	// What makes calling it every tick free. The caller asks whenever the set
	// changes and the answer is usually the same one; a version that cleared
	// unconditionally would throw away a map that was about to be rebuilt
	// identically.
	const std::vector<Proxy> proxies{At(0, 0.5f, 0.5f, 0.5f)};

	ChunkMap map(UNIT_CHUNK);
	map.Rebuild(proxies);
	map.SetChunkSize(UNIT_CHUNK);
	CHECK(map.ChunkCount() == 1);
}

TEST_CASE("a size at or below zero is refused in favour of the default", "[chunkmap]") {
	// The alternative is a reciprocal that is an infinity, and every chunk
	// coordinate after it is a wrong answer rather than a failure.
	CHECK(ChunkMap(0.0f).ChunkSize() == ChunkMap::DEFAULT_CHUNK_SIZE);
	CHECK(ChunkMap(-4.0f).ChunkSize() == ChunkMap::DEFAULT_CHUNK_SIZE);

	ChunkMap map(2.0f);
	map.SetChunkSize(-1.0f);
	CHECK(map.ChunkSize() == ChunkMap::DEFAULT_CHUNK_SIZE);
}

TEST_CASE("a suggested size cuts a flat scene into at least the wanted groups", "[chunkmap]") {
	// The case `SuggestChunkSize` is written against and the one a volume-based
	// estimate gets wrong: a tray of blocks is flat, so its volume is near zero
	// and a cube root of it would ask for a chunk near zero.
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 1024; index++) {
		const auto x = static_cast<float>(index % 32) * 4.0f;
		const auto z = static_cast<float>(index / 32) * 4.0f;
		proxies.push_back(Box(index, Vector3{x, 0.0f, z}, Vector3{x + 2.0f, 1.0f, z + 2.0f}));
	}

	const float size = SuggestChunkSize(proxies, 64);
	CHECK(size >= ChunkMap::MINIMUM_CHUNK_SIZE);
	CHECK(size <= ChunkMap::MAXIMUM_CHUNK_SIZE);

	ChunkMap map(size);
	map.Rebuild(proxies);

	// The estimate is a floor rather than a promise, because a real scene is not
	// spread evenly - but this one is, so it should land close.
	CHECK(map.ChunkCount() >= 32);
}

TEST_CASE("a suggested size is quantised so it stops moving", "[chunkmap]") {
	// The hysteresis, and it is `SuggestCellSize`'s argument unchanged: a size
	// computed exactly would differ every time a body was added and each
	// difference costs a full rebuild.
	std::vector<Proxy> proxies;
	for (uint64_t index = 0; index < 256; index++) {
		const auto step = static_cast<float>(index);
		proxies.push_back(At(index, step, 0.0f, step * 0.5f));
	}

	const float before = SuggestChunkSize(proxies, 16);
	proxies.push_back(At(256, 3.0f, 0.0f, 1.0f));
	CHECK(SuggestChunkSize(proxies, 16) == before);

	// A power of two, which is what the quantisation means.
	CHECK(std::exp2(std::round(std::log2(before))) == before);
}

TEST_CASE("a suggestion for one group asks for no subdivision", "[chunkmap]") {
	// The honest answer to "put everything in a group", and it costs nothing to
	// produce - a caller with one worker should not pay for a partition it will
	// not use.
	const std::vector<Proxy> proxies{At(0, 0.0f, 0.0f, 0.0f), At(1, 1000.0f, 0.0f, 0.0f)};
	CHECK(SuggestChunkSize(proxies, 1) == ChunkMap::MAXIMUM_CHUNK_SIZE);
	CHECK(SuggestChunkSize(proxies, 0) == ChunkMap::MAXIMUM_CHUNK_SIZE);
	CHECK(SuggestChunkSize({}, 16) == ChunkMap::DEFAULT_CHUNK_SIZE);
}

TEST_CASE("a suggestion over centres that are all one place takes the default", "[chunkmap]") {
	// There is nothing to cut and no size that would cut it. A zero extent
	// divided into groups is zero, and a chunk size of zero is the reciprocal
	// that reaches every later answer as a NaN.
	const std::vector<Proxy> proxies{At(0, 3.0f, 3.0f, 3.0f), At(1, 3.0f, 3.0f, 3.0f)};
	CHECK(SuggestChunkSize(proxies, 16) == ChunkMap::DEFAULT_CHUNK_SIZE);
}

TEST_CASE("a centre that is not a number is left out of the extent", "[chunkmap]") {
	// A world that has just been handed a garbage transform. The rejection is
	// written as a comparison against itself rather than as a `min`, because a
	// NaN compares false against everything and would otherwise poison the
	// extent and take the whole suggestion with it.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const std::vector<Proxy> proxies{
		At(0, 0.0f, 0.0f, 0.0f),
		At(1, nan, nan, nan),
		At(2, 256.0f, 0.0f, 256.0f),
	};

	const float size = SuggestChunkSize(proxies, 16);
	CHECK(size >= ChunkMap::MINIMUM_CHUNK_SIZE);
	CHECK(size <= ChunkMap::MAXIMUM_CHUNK_SIZE);

	// And the map itself still partitions, because the coordinate clamp puts a
	// NaN centre on the limit rather than casting it - which is undefined.
	ChunkMap map(size);
	map.Rebuild(proxies);
	CHECK(map.ProxyCount() == 3);
}
