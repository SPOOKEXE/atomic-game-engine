// What the broad phase costs, and how that moves with cell size.
//
// **This is where `HashGrid::DEFAULT_CELL_SIZE` comes from.** Broad-phase cost
// per 1000 colliders, and how it moves with cell size, is the question - and
// there is nowhere else for that number to live: the grid has no
// consumer until `physics` exists, so running the client tells nobody anything
// about it yet.
//
// Everything is reported per iteration, so a rebuild of 1000 proxies and a
// rebuild of 8000 are directly comparable and the per-collider figure is a
// division anybody can do in their head.
//
// The scene is a slab rather than a cube: colliders spread wide on X and Z and
// thin on Y, which is what a world of rooms and floors looks like and is the
// case a uniform grid is either good or bad at. A uniformly filled cube would
// flatter the structure.

#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/spatial/HashGrid.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.spatial.bench.hashgrid")

using engine::core::AABB;
using engine::core::Random;
using engine::core::Ray;
using engine::core::RayHit;
using engine::core::Vector3;
using engine::spatial::HashGrid;
using engine::spatial::LayerMask;
using engine::spatial::OverlapBox;
using engine::spatial::OverlapSphere;
using engine::spatial::Proxy;
using engine::spatial::Raycast;
using engine::spatial::RaycastAll;
using engine::spatial::ShapeCast;
using engine::testing::Consume;

namespace hashgrid_bench {
	// Half the width of the slab the scene fills, in metres.
	constexpr float SCENE_HALF_WIDTH = 128.0f;

	// Half its height. A world is wide and shallow.
	constexpr float SCENE_HALF_HEIGHT = 8.0f;

	// Half the median collider's edge, in metres, so the median collider is two
	// metres across. The design note's "about twice the median extent" would put
	// the cell size near four.
	constexpr float MEDIAN_EXTENT = 1.0f;

	// A scene of `count` colliders, built once and reused.
	//
	// Deterministic, through `core::Random`, which is indexed rather than
	// streamed - so two runs measure the same scene and a difference between
	// them is the code.
	const std::vector<Proxy> &SceneOf(size_t count) {
		static std::vector<std::pair<size_t, std::vector<Proxy>>> built;

		for (const auto &[size, proxies] : built) {
			if (size == count) {
				return proxies;
			}
		}

		std::vector<Proxy> proxies;
		proxies.reserve(count);
		for (size_t index = 0; index < count; index++) {
			const uint32_t seed = static_cast<uint32_t>(index);
			const Vector3 centre{
				Random::Range(seed, 3, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
				Random::Range(seed, 5, -SCENE_HALF_HEIGHT, SCENE_HALF_HEIGHT),
				Random::Range(seed, 7, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
			};
			// Spread around the median rather than all one size, so the grid is
			// not measured against colliders that all fit one cell exactly.
			const float extent = MEDIAN_EXTENT * Random::Range(seed, 11, 0.4f, 2.5f);
			proxies.push_back(
				Proxy{
					static_cast<uint64_t>(index + 1),
					AABB::FromCentre(centre, Vector3{extent, extent, extent}),
					LayerMask::Only(static_cast<uint32_t>(index % 4)),
				}
			);
		}

		built.emplace_back(count, std::move(proxies));
		return built.back().second;
	}

	// A grid over `count` colliders at `cellSize`, built once.
	//
	// Lazily rather than at static-initialisation time, so the measured body
	// never pays for the first rebuild.
	HashGrid &GridOf(size_t count, float cellSize) {
		static std::vector<std::pair<std::pair<size_t, float>, std::unique_ptr<HashGrid>>> built;

		for (auto &[key, grid] : built) {
			if (key.first == count && key.second == cellSize) {
				return *grid;
			}
		}

		auto grid = std::make_unique<HashGrid>(cellSize);
		grid->Rebuild(SceneOf(count));
		built.emplace_back(std::make_pair(count, cellSize), std::move(grid));
		return *built.back().second;
	}

	// A fixed set of rays across the scene, so every cell-size row answers the
	// same question.
	const std::vector<Ray> &Rays() {
		static const std::vector<Ray> rays = [] {
			std::vector<Ray> made;
			made.reserve(64);
			for (uint32_t index = 0; index < 64; index++) {
				const Vector3 origin{
					Random::Range(index, 101, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
					Random::Range(index, 103, -SCENE_HALF_HEIGHT, SCENE_HALF_HEIGHT),
					Random::Range(index, 107, -SCENE_HALF_WIDTH, SCENE_HALF_WIDTH),
				};
				const Vector3 direction =
					Vector3{
						Random::Range(index, 109, -1.0f, 1.0f),
						Random::Range(index, 113, -1.0f, 1.0f),
						Random::Range(index, 127, -1.0f, 1.0f),
					}
						.Unit();
				made.push_back(Ray{origin, direction == Vector3::Zero ? Vector3::XAxis : direction});
			}
			return made;
		}();
		return rays;
	}
}

using namespace hashgrid_bench;

// --- the rebuild, at three sizes ---------------------------------------------
//
// The grid is rebuilt from scratch every tick, so this is a per-tick cost and
// not a load-time one. Divide by the collider count for the figure §3.6 asks
// for.

BENCH("Rebuild · 1000 colliders, 4m cells", 200) {
	HashGrid grid{4.0f};
	for (int pass = 0; pass < 200; pass++) {
		grid.Rebuild(SceneOf(1000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 4000 colliders, 4m cells", 50) {
	HashGrid grid{4.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 16000 colliders, 4m cells", 20) {
	HashGrid grid{4.0f};
	for (int pass = 0; pass < 20; pass++) {
		grid.Rebuild(SceneOf(16000));
		Consume(grid.ProxyCount());
	}
}

// --- the first rebuild, which is the one that allocates ----------------------
//
// Every row above reuses the previous pass's storage, which is the steady state
// and the number that matters. This one is what the very first tick costs, and
// the gap between them is what the retained capacity is worth.

BENCH("Rebuild · 4000 colliders into a fresh grid", 50) {
	for (int pass = 0; pass < 50; pass++) {
		HashGrid grid{4.0f};
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

// --- how the rebuild moves with cell size ------------------------------------
//
// Smaller cells mean more entries per proxy and a longer build. The interesting
// half is what it buys on the query side below; read the two together.

BENCH("Rebuild · 4000 colliders, 1m cells", 50) {
	HashGrid grid{1.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 4000 colliders, 2m cells", 50) {
	HashGrid grid{2.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 4000 colliders, 8m cells", 50) {
	HashGrid grid{8.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 4000 colliders, 16m cells", 50) {
	HashGrid grid{16.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

BENCH("Rebuild · 4000 colliders, 32m cells", 50) {
	HashGrid grid{32.0f};
	for (int pass = 0; pass < 50; pass++) {
		grid.Rebuild(SceneOf(4000));
		Consume(grid.ProxyCount());
	}
}

// --- the overlap query, at the same cell sizes -------------------------------
//
// **This is the row the default cell size is chosen from.** An overlap of a
// body-sized box is what a broad phase runs per body per tick, so it is the
// query that multiplies. Bigger cells hold more candidates and cost more per
// query; smaller cells cost more to build. Where the two curves cross is the
// answer.

BENCH("OverlapBox · a 4m box, 4000 colliders, 1m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 1.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

BENCH("OverlapBox · a 4m box, 4000 colliders, 2m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 2.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

BENCH("OverlapBox · a 4m box, 4000 colliders, 4m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

BENCH("OverlapBox · a 4m box, 4000 colliders, 8m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 8.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

BENCH("OverlapBox · a 4m box, 4000 colliders, 16m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 16.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

BENCH("OverlapBox · a 4m box, 4000 colliders, 32m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 32.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::All(), found).Written);
	}
}

// --- the raycast, at the same cell sizes -------------------------------------
//
// Two lengths, because the two answer different questions. A gameplay ray is a
// few metres - a ground check, a melee reach - and a long one is a line of
// sight across the world.
//
// **The long row used to be dominated by the walk rather than by the
// candidates, and this is the row that paid for changing it.** A raycast asked
// the grid for everything inside the segment's bounding box, so halving the
// cell size multiplied the cells walked by eight while the candidates found
// stayed the same: 10.67 ms at 1 m cells against 26.40 us at 16 m, for one
// answer. `GridInternals::ForEachCandidateAlongRay` walks the line instead and
// the same row reads 12.90 us and 7.09 us, so the cost is now linear in the
// ray's length and the default no longer has to be chosen around it.
//
// Keep both ends of the sweep. The cheap fine-cell number is the whole evidence
// that the walk is linear, and a benchmark trimmed to the cell sizes somebody
// currently ships stops being able to show that.

BENCH("Raycast · 64 rays, 4000 colliders, 1m cells", 200) {
	const HashGrid &grid = GridOf(4000, 1.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays, 4000 colliders, 2m cells", 200) {
	const HashGrid &grid = GridOf(4000, 2.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays, 4000 colliders, 4m cells", 200) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays, 4000 colliders, 8m cells", 200) {
	const HashGrid &grid = GridOf(4000, 8.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays, 4000 colliders, 16m cells", 200) {
	const HashGrid &grid = GridOf(4000, 16.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays, 4000 colliders, 32m cells", 200) {
	const HashGrid &grid = GridOf(4000, 32.0f);
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 64.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays over 8m, 4000 colliders, 2m cells", 500) {
	const HashGrid &grid = GridOf(4000, 2.0f);
	for (int pass = 0; pass < 500; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 8.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays over 8m, 4000 colliders, 4m cells", 500) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	for (int pass = 0; pass < 500; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 8.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays over 8m, 4000 colliders, 8m cells", 500) {
	const HashGrid &grid = GridOf(4000, 8.0f);
	for (int pass = 0; pass < 500; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 8.0f).has_value());
		}
	}
}

BENCH("Raycast · 64 rays over 8m, 4000 colliders, 16m cells", 500) {
	const HashGrid &grid = GridOf(4000, 16.0f);
	for (int pass = 0; pass < 500; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(Raycast(grid, ray, 8.0f).has_value());
		}
	}
}

// A long diagonal sweep is the shape whose bounding box is misleading. At one
// metre cells, the old volume walk reached the scan fallback instead of opening
// only the neighbourhood of the path.
BENCH("ShapeCast · 64 1m boxes over 64m, 4000 colliders, 1m cells", 200) {
	const HashGrid &grid = GridOf(4000, 1.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			const AABB box = AABB::FromCentre(ray.Origin, Vector3{0.5f, 0.5f, 0.5f});
			Consume(ShapeCast(grid, box, ray.Direction * 64.0f, LayerMask::All(), found).Written);
		}
	}
}

// --- the other queries, at one cell size -------------------------------------
//
// The sweep is what a character controller runs and the sphere is what a
// trigger runs. Both at 4 metres, so they are comparable with the box row
// above.

BENCH("OverlapSphere · a 4m sphere, 4000 colliders, 4m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		Consume(OverlapSphere(grid, Vector3{offset, 0.0f, offset}, 4.0f, LayerMask::All(), found).Written);
	}
}

BENCH("ShapeCast · a 1m box over 8m, 4000 colliders, 4m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{0.5f, 0.5f, 0.5f});
		Consume(ShapeCast(grid, box, Vector3{8.0f, 0.0f, 0.0f}, LayerMask::All(), found).Written);
	}
}

BENCH("RaycastAll · 64 rays, 4000 colliders, 4m cells", 200) {
	// Against `Raycast` at the same size, this is what keeping every hit sorted
	// costs over keeping the nearest one.
	const HashGrid &grid = GridOf(4000, 4.0f);
	std::array<RayHit, 16> hits{};
	for (int pass = 0; pass < 200; pass++) {
		for (const Ray &ray : Rays()) {
			Consume(RaycastAll(grid, ray, 64.0f, LayerMask::All(), hits).Written);
		}
	}
}

// --- what a layer mask is worth ----------------------------------------------
//
// A mask matching one layer in four rejects three candidates out of four before
// the box test. What that saves is the reason the layers are in the proxy
// rather than looked up per candidate.

BENCH("OverlapBox · one layer in four, 4000 colliders, 4m cells", 2000) {
	const HashGrid &grid = GridOf(4000, 4.0f);
	std::array<uint64_t, 64> found{};
	for (int pass = 0; pass < 2000; pass++) {
		const float offset = static_cast<float>(pass % 64) - 32.0f;
		const AABB box = AABB::FromCentre(Vector3{offset, 0.0f, offset}, Vector3{2.0f, 2.0f, 2.0f});
		Consume(OverlapBox(grid, box, LayerMask::Only(1), found).Written);
	}
}
