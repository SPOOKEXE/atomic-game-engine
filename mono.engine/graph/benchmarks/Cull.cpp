// What culling costs, and what it saves.
//
// **The one optimisation that changes what is correct**, so the number that
// matters is not only how fast the test is but how much of the world it
// removes. Both are here: the per-instance cost, and the visible fraction for a
// camera framing its own scene against one inside a large world.
//
// Six planes, six dot products, and an early reject on the first plane that
// excludes - so the cost of a *rejected* instance is lower than an accepted one
// and the ratio moves with the scene rather than being flat.
//
// What it measured, in the `bench` preset, on a 24-thread machine. Minimum
// sample per call, with the spread beside it:
//
// | Row | Cost | Per instance |
// |---|---|---|
// | Extract a frustum | 72 ns ± 8% | |
// | Cull 1000, all visible | 7.25 us ± 2% | 7.3 ns |
// | Cull 1000, all behind | 3.18 us ± 2% | 3.2 ns |
// | Cull and bound 1000, all visible | 7.90 us ± 2% | 7.9 ns |
// | Cull and bound 10000, mixed | 79.1 us ± 2% | 7.9 ns |
// | Cull 10000, mixed | 75.2 us ± 1% | 7.5 ns |
// | Cull 20000, mixed, pooled | 123 us ± 66% | 6.1 ns |
// | Cull 100000, mixed, pooled | 263 us ± 30% | 2.6 ns |
// | Bound a rotated instance | 9 ns ± 1 | |
//
// The same 20,000 and 100,000 rows ran inline at 344 us and 1.76 ms before the
// benchmark started the worker pool. The 16,384-row handover keeps the small
// cases unchanged and cuts the large case by about four times. The wide spread
// is scheduler noise across twenty-three workers, not a stable per-row cost.
// Forcing the fused 10,000-row walk through that pool measured 98.6 us against
// 79.1 us inline, so lowering its crossover would make the Studio-sized case
// about one quarter slower.
//
// **Axis-aligned bounds are the common case and cost no quaternion work.** The
// identity path cut the thousand-row visible case from 17.23 us to 7.25 us and
// the fused cull-and-bound case from 17.38 us to 7.90 us. A rotated bound still
// costs 9 ns. If a heavily rotated scene makes this row expensive, caching that
// bound behind transform changes is the next measured move.

// Rejecting is cheaper because the plane loop stops early. The bound is still
// paid first, so a rotated rejected row cannot be as cheap as an axis-aligned
// one.
//
// Extraction is once per view per frame and is not worth thinking about.
//
// A number is reported, never enforced.

#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/parallel/Jobs.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/testing/Bench.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.graph.bench.cull")

using engine::core::CFrame;
using engine::core::Vector3;
using engine::graph::Cull;
using engine::graph::Frustum;
using engine::scene::Camera;
using engine::scene::DrawInstance;
using engine::testing::Consume;

namespace cull_bench {
	struct Pool {
		Pool() {
			engine::parallel::Jobs::Start(0);
		}
		~Pool() {
			engine::parallel::Jobs::Stop();
		}
	};
	void StartWorkers() {
		static const Pool workers;
		(void)workers;
	}

	Frustum ViewFrom(const Vector3 &position) {
		Camera camera;
		return Frustum::FromViewProjection(
			engine::scene::ResolveCamera(CFrame{position}, camera, 16.0f / 9.0f).ViewProjection
		);
	}

	// A scene laid out along -Z, which is forward. `spread` decides how much of
	// it a camera at the origin can see.
	std::vector<DrawInstance> &SceneOf(size_t count, float spread) {
		static std::vector<DrawInstance> instances;
		static size_t builtCount = 0;
		static float builtSpread = 0.0f;

		if (builtCount == count && builtSpread == spread) {
			return instances;
		}

		instances.clear();
		instances.reserve(count);

		for (size_t index = 0; index < count; index++) {
			// Deterministic placement rather than a generator: a benchmark whose
			// scene differs between runs is measuring two things.
			const auto step = static_cast<float>(index);
			DrawInstance instance;
			instance.Frame = CFrame{Vector3{
				std::fmod(step * 7.0f, 40.0f) - 20.0f, std::fmod(step * 3.0f, 20.0f) - 10.0f, spread * step
			}};
			instances.push_back(instance);
		}

		builtCount = count;
		builtSpread = spread;
		return instances;
	}
}

using namespace cull_bench;

BENCH("Extract a frustum", 200000) {
	for (int pass = 0; pass < 200000; pass++) {
		const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, static_cast<float>(pass % 4)});
		Consume(frustum.Planes[0].Distance);
	}
}

BENCH("Cull 1000, all visible", 2000) {
	// The case culling buys nothing, so it must cost almost nothing.
	const auto &instances = SceneOf(1000, -0.05f);
	const Frustum frustum = ViewFrom(Vector3::Zero);

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 2000; pass++) {
		Consume(Cull(instances, frustum, visible));
	}
}

BENCH("Cull 1000, all behind", 2000) {
	// Every instance fails the near plane and never sees the other five.
	const auto &instances = SceneOf(1000, 0.05f);
	const Frustum frustum = ViewFrom(Vector3::Zero);

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 2000; pass++) {
		Consume(Cull(instances, frustum, visible));
	}
}

BENCH("Cull and bound 1000, all visible", 2000) {
	// StressParticles' shape: a modest, mostly axis-aligned object field where
	// the directional-light bound is built beside the camera whitelist.
	const auto &instances = SceneOf(1000, -0.05f);
	const Frustum frustum = ViewFrom(Vector3::Zero);

	std::vector<uint32_t> visible;
	engine::core::AABB bounds;
	for (int pass = 0; pass < 2000; pass++) {
		Consume(engine::graph::CullAndBound(instances, frustum, visible, bounds));
	}
}

BENCH("Cull and bound 10000, mixed", 500) {
	const auto &instances = SceneOf(10000, -0.02f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	engine::core::AABB bounds;
	for (int pass = 0; pass < 500; pass++) {
		Consume(engine::graph::CullAndBound(instances, frustum, visible, bounds));
	}
}

BENCH("Cull and bound 20000, mixed", 300) {
	StartWorkers();
	const auto &instances = SceneOf(20000, -0.01f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	engine::core::AABB bounds;
	for (int pass = 0; pass < 300; pass++) {
		Consume(engine::graph::CullAndBound(instances, frustum, visible, bounds));
	}
}

BENCH("Cull 10000, mixed", 500) {
	const auto &instances = SceneOf(10000, -0.02f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 500; pass++) {
		Consume(Cull(instances, frustum, visible));
	}
}

BENCH("Cull 20000, mixed", 300) {
	StartWorkers();
	const auto &instances = SceneOf(20000, -0.01f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 300; pass++) {
		Consume(Cull(instances, frustum, visible));
	}
}

BENCH("Cull 100000, mixed", 100) {
	StartWorkers();
	const auto &instances = SceneOf(100000, -0.002f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 100; pass++) {
		Consume(Cull(instances, frustum, visible));
	}
}

BENCH("Bound a rotated instance", 200000) {
	DrawInstance turned;
	turned.Frame = CFrame::Angles(0.3f, 0.7f, 0.1f);

	for (int pass = 0; pass < 200000; pass++) {
		Consume(engine::graph::BoundsOf(turned).Size().X);
	}
}
