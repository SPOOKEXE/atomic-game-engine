// Measure synchronous removal fan-out without including hierarchy construction.
// Run the same fixture against both implementations when comparing changes.

#include <engine/core/HeapProfile.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/testing/Bench.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

TEST_SUITE_ID("engine.ecs.bench.removal")

namespace {
	using engine::ecs::Classes;
	using engine::ecs::Entity;
	using engine::ecs::NULL_ENTITY;
	using engine::ecs::Store;

	uint64_t PairHash(Entity ancestor, Entity subject) {
		return (ancestor.Id * UINT64_C(0x9e3779b185ebca87)) ^ (subject.Id * UINT64_C(0xc2b2ae3d27d4eb4f));
	}

	void Run(size_t depth, bool listening) {
		Store store("removal-bench");
		static const auto kind = Classes::Register("bench.RemovalNode", {});
		std::vector<Entity> ancestors;
		std::vector<Entity> subjects;
		ancestors.reserve(depth);
		subjects.reserve(depth);

		Entity parent = NULL_ENTITY;
		for (size_t index = 0; index < depth; ++index) {
			const Entity node = store.CreateInstance(kind, "ancestor");
			if (node == NULL_ENTITY || !store.SetParent(node, parent))
				throw std::runtime_error("fixture ancestor");
			ancestors.push_back(node);
			parent = node;
		}
		for (size_t index = 0; index < depth; ++index) {
			const Entity node = store.CreateInstance(kind, "subject");
			if (node == NULL_ENTITY || !store.SetParent(node, parent))
				throw std::runtime_error("fixture subject");
			subjects.push_back(node);
			parent = node;
		}

		uint64_t count = 0;
		uint64_t digest = 0;
		if (listening) {
			store.OnDescendantRemoving([&](Entity ancestor, Entity subject) {
				++count;
				digest += PairHash(ancestor, subject);
			});
		}
		uint64_t expectedDigest = 0;
		for (const Entity ancestor : ancestors) {
			for (const Entity subject : subjects)
				expectedDigest += PairHash(ancestor, subject);
		}
		for (int trial = 0; trial < 3; ++trial) {
			count = 0;
			digest = 0;
			const auto start = std::chrono::steady_clock::now();
			const bool detached = store.SetParent(subjects.front(), NULL_ENTITY);
			const auto elapsed = std::chrono::steady_clock::now() - start;
			if (!detached || store.ParentOf(subjects.front()) != NULL_ENTITY ||
				count != (listening ? depth * depth : 0) || digest != (listening ? expectedDigest : 0)) {
				throw std::runtime_error("removal event oracle failed");
			}
			std::printf(
				"# removal\tdepth=%zu\tlistener=%d\ttrial=%d\tevents=%llu\tdigest=%llu\tdetach_ns=%lld\n",
				depth,
				listening ? 1 : 0,
				trial,
				static_cast<unsigned long long>(count),
				static_cast<unsigned long long>(digest),
				static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count())
			);
			if (!store.SetParent(subjects.front(), ancestors.back()))
				throw std::runtime_error("fixture reattach");
		}
	}
}

BENCH("deep removal fan-out", 1) {
	const char *tree = std::getenv("MONO_REMOVAL_BUILD_TREE");
	std::printf(
		"# removal basis\tbuild_tree=%s\theap_hooks=%d\tchecksum=commutative_pair_sum\ttiming_qualified=0\n",
		tree == nullptr ? "unspecified" : tree,
		engine::core::HeapProfile::IsCompiledIn() ? 1 : 0
	);
	for (const size_t depth : {size_t{16}, size_t{64}, size_t{128}, size_t{256}}) {
		Run(depth, false);
		Run(depth, true);
	}
}
