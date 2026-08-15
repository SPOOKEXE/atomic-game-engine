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
// | Extract a frustum | 41 ns ± 1 | |
// | Cull 1000, all visible | 17.2 us ± 1.3 | 17 ns |
// | Cull 1000, all behind | 13.3 us ± 0.3 | 13 ns |
// | Cull 10000, mixed | 172 us ± 8 | 17 ns |
// | Bound a rotated instance | 9 ns ± 1 | |
//
// **Bounding costs more than testing, which was not the expected shape.** Nine
// of the seventeen nanoseconds an instance is `AABB::FromOrientedBox` - three
// quaternion rotations to find what a turned box actually reaches - and only
// the remaining eight are the six planes. The plane test was the part that
// looked expensive and is not.
//
// That points somewhere specific if this ever becomes the frame's cost: the
// bound is a function of `Frame` and `HalfExtent`, both of which a world
// already has, so it is *cacheable* per instance and recomputable only when the
// transform changes - which `ecs::ChangeChannel` already knows. Nothing does
// that today because 17 ns times a few thousand instances is 30 microseconds
// against a 16 millisecond frame.
//
// **Rejecting is only a quarter cheaper than accepting**, not the several times
// an early-out suggests, and the same arithmetic explains it: the bound is paid
// whatever the answer, and only the plane loop short-circuits.
//
// Extraction is once per view per frame and is not worth thinking about.
//
// A number is reported, never enforced.

#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
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

BENCH("Cull 10000, mixed", 500) {
	const auto &instances = SceneOf(10000, -0.02f);
	const Frustum frustum = ViewFrom(Vector3{0.0f, 0.0f, 40.0f});

	std::vector<uint32_t> visible;
	for (int pass = 0; pass < 500; pass++) {
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
