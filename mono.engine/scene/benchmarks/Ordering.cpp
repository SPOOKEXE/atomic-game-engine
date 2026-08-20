// What ordering a scene costs, per view, per frame.
//
// **The multiplier here is views, and it is easy to forget.** `OrderScene` runs
// once for the player's camera, once for every shadow-casting light, and once
// for every live surface - and `MAX_SURFACES` is sixteen. A room with four
// mirrors and a sun is six full orderings of the same draw list every frame, so
// a figure that looks fine on its own is six times that in the frame budget.
//
// **Transparency is the parameter that changes the shape, not the size.** An
// opaque scene is a partition - a linear pass - and a transparent one is a
// stable sort by squared distance, which is `n log n` and allocates. So the
// ladder below varies the transparent *fraction* at a fixed instance count, and
// the gap between 0% and 25% is the whole cost of the blended pass's ordering.
// A scene author who fills a wall with glass panes is walking up that ladder
// without being told, and this is where the price is written down.
//
// `SignatureOf` is measured because it runs over the whole list every frame per
// surface to decide whether that surface needs redrawing at all. It is the
// cheap check guarding an expensive redraw, and a cheap check that is not cheap
// is worse than no check - it pays the scan and then redraws anyway.

#include <engine/core/Name.hpp>
#include <engine/core/Random.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.scene.bench.ordering")

using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Random;
using engine::core::Vector3;
using engine::scene::DrawInstance;
using engine::scene::GroupSurfaces;
using engine::scene::IsTransparent;
using engine::scene::MAX_SURFACES;
using engine::scene::OrderForDrawing;
using engine::scene::OrderScene;
using engine::scene::PartitionCasters;
using engine::scene::PartitionSurfaces;
using engine::scene::ScenePlan;
using engine::scene::SignatureOf;
using engine::scene::SurfaceRun;
using engine::testing::Consume;

namespace ordering_bench {

	// Half the width of the room the scene fills. The same slab shape
	// `spatial`'s suite uses, and for the same reason: a world is wide and
	// shallow, and a uniformly filled cube would flatter a distance sort by
	// spreading the keys further apart than a real scene does.
	constexpr float HALF_WIDTH = 128.0f;
	constexpr float HALF_HEIGHT = 8.0f;

	// Where the view sits. Off to one side rather than at the origin, so the
	// distance keys are not symmetric about zero - a symmetric spread halves
	// the number of distinct keys a sort has to separate.
	const Vector3 EYE(96.0f, 4.0f, 96.0f);

	// A draw list of `count` instances, `transparentPercent` of which are
	// blended, `surfacePercent` of which show a mirror, built once and reused.
	//
	// Deterministic through `core::Random`, so two runs order the same scene and
	// a difference between them is the code rather than the data.
	const std::vector<DrawInstance> &
	SceneOf(size_t count, uint32_t transparentPercent, uint32_t surfacePercent) {
		static std::vector<std::pair<std::array<size_t, 3>, std::vector<DrawInstance>>> built;
		const std::array<size_t, 3> key{count, transparentPercent, surfacePercent};
		for (const auto &[made, instances] : built) {
			if (made == key) {
				return instances;
			}
		}

		static const Name mesh("engine.bench.scene.Mesh");

		std::vector<DrawInstance> made;
		made.reserve(count);
		for (uint32_t index = 0; index < count; index++) {
			DrawInstance instance;
			instance.Frame = CFrame(Vector3(
				Random::Range(index, 3, -HALF_WIDTH, HALF_WIDTH),
				Random::Range(index, 5, -HALF_HEIGHT, HALF_HEIGHT),
				Random::Range(index, 7, -HALF_WIDTH, HALF_WIDTH)
			));
			instance.HalfExtent = Vector3(1.0f, 1.0f, 1.0f);
			instance.Tint = Color3(1.0f, 1.0f, 1.0f);
			instance.Mesh = mesh;

			const uint32_t roll = Random::Bits(index, 11) % 100u;
			instance.Transparency = roll < transparentPercent ? 0.5f : 0.0f;

			const uint32_t surfaceRoll = Random::Bits(index, 13) % 100u;
			instance.Surface = surfaceRoll < surfacePercent
								   ? static_cast<int16_t>(Random::Bits(index, 17) % MAX_SURFACES)
								   : static_cast<int8_t>(-1);

			// Three casters in four, which is what a scene with some decoration
			// and some structure looks like. All-or-nothing would make
			// `PartitionCasters` take its early-out and measure the scan.
			instance.CastShadow = (Random::Bits(index, 19) % 4u) != 0u;

			made.push_back(instance);
		}

		built.emplace_back(key, std::move(made));
		return built.back().second;
	}

	// The order buffer, held across frames the way a renderer holds one.
	//
	// **`OrderScene` resizes it rather than allocating a new one**, so a
	// benchmark constructing a fresh vector per sample would measure the
	// allocator and report a number no frame ever pays.
	std::vector<uint32_t> &Order() {
		static std::vector<uint32_t> order;
		return order;
	}
}

using namespace ordering_bench;

// --- the whole ordering, by scene size -----------------------------------------
//
// One iteration is one instance, so every row divides into a per-instance cost
// and the ladder says directly whether ordering is linear in the draw list.

BENCH_PER_ITEM("OrderScene · 1k instances, opaque", 1000) {
	const std::vector<DrawInstance> &instances = SceneOf(1000, 0, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Opaque);
}

BENCH_PER_ITEM("OrderScene · 10k instances, opaque", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Opaque);
}

BENCH_PER_ITEM("OrderScene · 50k instances, opaque", 50'000) {
	// A large scene, and the one that says whether the per-instance cost is
	// really flat. Fifty thousand draw instances is more than a renderer should
	// be submitting, which is exactly why it belongs here: the number that says
	// "do not do this" has to exist before anybody asks.
	const std::vector<DrawInstance> &instances = SceneOf(50'000, 0, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Opaque);
}

// --- the transparency ladder ---------------------------------------------------
//
// **Same instance count, more of them blended.** The opaque head is partitioned
// in linear time; the blended tail is stable-sorted by squared distance. So the
// climb across these four rows is the whole cost of the second pass's ordering,
// and it is superlinear in a way none of the rows above can show.

BENCH_PER_ITEM("OrderScene · 10k instances, 0% transparent", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Transparent);
}

BENCH_PER_ITEM("OrderScene · 10k instances, 5% transparent", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 5, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Transparent);
}

BENCH_PER_ITEM("OrderScene · 10k instances, 25% transparent", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 25, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Transparent);
}

BENCH_PER_ITEM("OrderScene · 10k instances, 100% transparent", 10'000) {
	// **The worst case, and it is a real scene**: a wall of glass, a particle
	// field, a UI layer drawn in world space. Everything sorts, nothing
	// partitions, and this is what that frame costs before a single triangle is
	// submitted.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 100, 0);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Transparent);
}

// --- mirrors -------------------------------------------------------------------

BENCH_PER_ITEM("OrderScene · 10k instances, 10% showing a surface", 10'000) {
	// `PartitionSurfaces` returns without touching the order when nothing shows
	// a surface - every scene with no mirror in it - because `stable_partition`
	// allocates a temporary. So the gap between this row and the 0% one is the
	// difference between a per-frame heap allocation in the render path and
	// none, which is what that early-out was written for.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 10);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Surfaces);
}

BENCH_PER_ITEM("OrderScene · 10k instances, 10% surfaces and 10% transparent", 10'000) {
	// Both passes doing work at once, which is what a hall of mirrors with
	// windows actually is. Read against the two single-cause rows: if it is
	// dearer than their sum, the two orderings are interfering.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 10, 10);
	const ScenePlan plan = OrderScene(instances, EYE, Order());
	Consume(plan.Surfaces + plan.Transparent);
}

BENCH_PER_ITEM("GroupSurfaces · 10k instances, 10% across 16 indices", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 10);
	std::vector<uint32_t> &order = Order();
	const ScenePlan plan = OrderScene(instances, EYE, order);

	SurfaceRun runs[MAX_SURFACES]{};
	const std::span<uint32_t> mirrors(order.data() + plan.Reflected, plan.Opaque - plan.Reflected);
	GroupSurfaces(instances, mirrors, plan.Reflected, true, runs);
	Consume(runs[0].OpaqueCount);
}

// --- the pieces ----------------------------------------------------------------
//
// Measured separately so a regression in the whole can be attributed rather
// than merely noticed.

BENCH_PER_ITEM("OrderForDrawing · 10k instances, 25% transparent", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 25, 0);
	Consume(OrderForDrawing(instances, EYE, Order()));
}

BENCH_PER_ITEM("PartitionCasters · 10k opaque instances", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 0);
	std::vector<uint32_t> &order = Order();
	order.resize(instances.size());
	for (uint32_t index = 0; index < order.size(); index++) {
		order[index] = index;
	}
	Consume(PartitionCasters(instances, order));
}

BENCH_PER_ITEM("PartitionSurfaces · 10k instances with no mirror", 10'000) {
	// **The early-out row.** No instance shows a surface, so this must return
	// without touching the order and without allocating. It should be a linear
	// scan and nothing else - if it costs anything like the row below, the scan
	// that avoids `stable_partition`'s temporary is not happening.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 0);
	std::vector<uint32_t> &order = Order();
	order.resize(instances.size());
	for (uint32_t index = 0; index < order.size(); index++) {
		order[index] = index;
	}
	Consume(PartitionSurfaces(instances, order));
}

BENCH_PER_ITEM("PartitionSurfaces · 10k instances, 10% showing a mirror", 10'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 0, 10);
	std::vector<uint32_t> &order = Order();
	order.resize(instances.size());
	for (uint32_t index = 0; index < order.size(); index++) {
		order[index] = index;
	}
	Consume(PartitionSurfaces(instances, order));
}

BENCH_PER_ITEM("IsTransparent · 100k calls", 100'000) {
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 25, 0);
	uint32_t blended = 0;
	for (size_t index = 0; index < 100'000; index++) {
		blended += IsTransparent(instances[index % instances.size()]) ? 1u : 0u;
	}
	Consume(blended);
}

// --- the redraw check ----------------------------------------------------------

BENCH_PER_ITEM("SignatureOf · 10k instances", 10'000) {
	// **Runs every frame per live surface**, over the whole list, to decide
	// whether that surface needs redrawing. Sixteen surfaces is sixteen full
	// scans of the draw list before anything is drawn - so this row times
	// `MAX_SURFACES` is the fixed cost of the reflection system in a frame where
	// nothing moved and nothing was redrawn.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 10, 10);
	Consume(SignatureOf(instances));
}

BENCH_PER_ITEM("SignatureOf · 50k instances", 50'000) {
	const std::vector<DrawInstance> &instances = SceneOf(50'000, 10, 10);
	Consume(SignatureOf(instances));
}

// --- a frame -------------------------------------------------------------------

BENCH_PER_ITEM("frame · 10k instances ordered for 6 views", 6) {
	// **The real bill: a camera, a sun and four mirrors.** One iteration is one
	// view, so the figure is the per-view cost and the row total is what a frame
	// spends ordering before the renderer has issued a command. At 60 Hz a frame
	// is 16.7 ms; this is how much of it goes on deciding what order to draw
	// things in.
	const std::vector<DrawInstance> &instances = SceneOf(10'000, 10, 10);
	std::vector<uint32_t> &order = Order();

	uint32_t total = 0;
	for (size_t view = 0; view < 6; view++) {
		// Each view sees from somewhere else, so no sort can be reused and the
		// distance keys are genuinely recomputed.
		const Vector3 eye(EYE.X + static_cast<float>(view) * 32.0f, EYE.Y, EYE.Z);
		const ScenePlan plan = OrderScene(instances, eye, order);
		total += plan.Opaque + plan.Transparent;
	}
	Consume(total);
}
