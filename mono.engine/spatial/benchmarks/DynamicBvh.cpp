#include <engine/core/types/AABB.hpp>
#include <engine/spatial/DynamicBvh.hpp>
#include <engine/spatial/Query.hpp>
#include <engine/testing/Bench.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.spatial.bench.dynamicbvh")

using engine::core::AABB;
using engine::core::Vector3;
using engine::spatial::DynamicBvh;
using engine::spatial::LayerMask;
using engine::spatial::OverlapBox;
using engine::spatial::Proxy;
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
