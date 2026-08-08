// What a frame costs the CPU before the device sees any of it.
//
// **There was no before-number for a frame in this repository**, and v0.11
// spends the version rearranging what a frame is. `RENDER_PIPELINE.md` §17 is
// blunt about why that matters: *"a graph can be slower than the code it
// replaces. Per-node dispatch, hashing and cache lookups are real overhead, and
// at the current profile added CPU work is exactly the wrong direction."* A
// claim like that is only checkable against a measurement taken first.
//
// **Why this measures no GPU, and is still a frame benchmark.**
// `benchmarks/Overlay.cpp` states the constraint this file lives under —
// everything past the upload belongs to the driver and the display. So what is
// measured here is the part of `Renderer::Render` that is arithmetic: resolve
// the camera, extract the frustum, cull, bound, copy the survivors, order them,
// partition the mirrors, group them, sign the list, order the scene range. That
// is every line of the function above the first `SDL_BeginGPURenderPass`, and
// on a frame that draws nothing new it is very nearly the whole of it.
//
// **It composes rather than duplicates.** `graph/benchmarks/Cull.cpp` measures
// culling and `scene/benchmarks/Ordering.cpp` measures ordering; each is honest
// about its own piece and neither can see the sum. `Ordering.cpp`'s header
// names the gap exactly: *"The multiplier here is views, and it is easy to
// forget."* Nobody had multiplied it.
//
// ## The two questions this exists to answer
//
// **What does the Nth viewport cost?** v0.7 decided the studio would draw N
// panels in N `Render` calls round-robin, and wrote down the condition for
// revisiting it: *"when a viewport is expensive enough that its record is
// measurable."* That was never measured, so the decision has been standing on
// an assumption for four versions. The 1/2/4 ladder below is that measurement.
//
// **How much of a view's record is not the view's?** This is `Node::PerView`
// stated as a number. A frame today calls `graph::CullAndBound` once per view,
// and that function fuses two walks: the cull, which is per view, and the union
// bound the shadow fit needs, which is over the *whole* draw list and therefore
// identical for every view of one world. Four views compute one identical bound
// four times, so hoisting it into a shared node looks like free money.
//
// It is not, and that is the most useful thing in this file.
//
// ## What it measured, in the `bench` preset
//
// Minimum sample per frame recorded, with the spread beside it. One iteration
// is one frame.
//
// | Row | Cost | Per view |
// |---|---|---|
// | Extract a frustum | 42 ns ± 1 | |
// | Record one view, 5000 instances | 145 us ± 3 | 145 us |
// | Record two views, 5000 instances | 298 us ± 17 | 149 us |
// | Record four views, 5000 instances | 617 us ± 54 | 154 us |
// | Record four views, bound hoisted | 680 us ± 11 | 170 us |
// | Record one view, 1000 instances | 27 us ± 0.4 | |
// | Record one view, 20000 instances | 782 us ± 23 | |
//
// The frustum row is 42 ns against the 41 ns `graph/benchmarks/Cull.cpp`
// measured independently, which is the cross-check that says these rows are
// counting what they claim to.
//
// **A viewport costs about 150 microseconds of CPU record, and that answers
// v0.7.** The condition written down then was *"when a viewport is expensive
// enough that its record is measurable"* — it is measurable, comfortably. Four
// panels are 617 us: 3.7% of a 60 fps frame, but **18% of a 300 fps one**,
// which is the rate this engine actually runs at. The round-robin decision
// stands, and now it stands on a number.
//
// **Hoisting the bound out of the cull made the frame 10% slower**, which is
// the opposite of what this row was added to confirm. The reason is already
// written down one module over: `Cull.cpp`'s table records that nine of a
// culled instance's seventeen nanoseconds are `AABB::FromOrientedBox` and only
// eight are the six planes. The cull *needs* that box whether or not anyone
// wants the union — so `CullAndBound` fusing them is the optimisation, the
// union itself is six comparisons on a box that already exists, and hoisting it
// buys those six comparisons at the price of a second walk deriving the same
// five thousand boxes. Measured: about 63 us of extra work to save almost none.
//
// **So the shareable thing is the shadow *pass*, not the shadow *fit*.** A
// `PerView = false` node earns its keep by not re-rendering a shadow map — real
// draw calls over the caster range — and not by hoisting arithmetic the cull
// was doing anyway. `graph::StandardGraph` marks the pass shared and says
// nothing about the bound, which this says is the right line.
//
// A number is reported, never enforced — the repository's rule for every
// benchmark in it.

#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Color3.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/graph/Cull.hpp>
#include <engine/graph/Frustum.hpp>
#include <engine/graph/Shadow.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

TEST_SUITE_ID("engine.render.bench.frame")

using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Color3;
using engine::core::Name;
using engine::core::Random;
using engine::core::Vector3;
using engine::graph::Cull;
using engine::graph::CullAndBound;
using engine::graph::FitDirectionalLight;
using engine::graph::Frustum;
using engine::scene::Camera;
using engine::scene::DrawInstance;
using engine::scene::GroupSurfaces;
using engine::scene::MAX_SURFACES;
using engine::scene::OrderForDrawing;
using engine::scene::OrderScene;
using engine::scene::PartitionSurfaces;
using engine::scene::ResolveCamera;
using engine::scene::ScenePlan;
using engine::scene::SignatureOf;
using engine::scene::SurfaceRun;
using engine::testing::Consume;

namespace frame_bench {

	// The slab a world actually occupies — wide, shallow, and the same shape
	// `scene`'s and `spatial`'s suites use. A uniformly filled cube would
	// flatter both the cull and the distance sort by spreading instances
	// further apart than a room does.
	constexpr float HALF_WIDTH = 128.0f;
	constexpr float HALF_HEIGHT = 8.0f;

	// The sun, matching `render`'s own `SUN_DIRECTION` in shape: down and to one
	// side. A light straight down makes the fit's extents axis-aligned, which is
	// the one case its arithmetic is cheapest on.
	const Vector3 SUN(-0.45f, -0.85f, -0.27f);

	// A draw list of `count` instances, built once and reused across samples.
	//
	// Deterministic through `core::Random`, so two runs record the same world
	// and a difference between them is this code rather than the data.
	const std::vector<DrawInstance> &SceneOf(size_t count) {
		// **A `deque` rather than a `vector`, and the difference is not style.**
		// Growing a vector of scenes moves every element, which invalidates
		// every reference this function has already handed out — and it hands
		// out references by design, because the alternative is copying a
		// twenty-thousand-instance draw list into each sample. A deque never
		// moves what it already holds.
		static std::deque<std::pair<size_t, std::vector<DrawInstance>>> built;
		for (const auto &[made, instances] : built) {
			if (made == count) {
				return instances;
			}
		}

		static const Name mesh("engine.bench.frame.Mesh");

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

			// A tenth blended and a twentieth reflective: a room with some glass
			// and a mirror or two, rather than either degenerate end. Both
			// fractions change the *shape* of the work and not just its size —
			// zero transparency turns the sort into a partition and zero
			// surfaces lets `PartitionSurfaces` take its early-out.
			instance.Transparency = (Random::Bits(index, 11) % 100u) < 10u ? 0.5f : 0.0f;
			instance.Surface = (Random::Bits(index, 13) % 100u) < 5u
								   ? static_cast<int8_t>(Random::Bits(index, 17) % MAX_SURFACES)
								   : static_cast<int8_t>(-1);

			// Three casters in four, so the caster partition does real work
			// rather than measuring its own scan.
			instance.CastShadow = (Random::Bits(index, 19) % 4u) != 0u;

			made.push_back(instance);
		}

		built.emplace_back(count, std::move(made));
		return built.back().second;
	}

	// The buffers a renderer holds across frames, held across frames here too.
	//
	// **Reused rather than constructed per sample, and that is not a
	// convenience.** `Impl` keeps exactly these vectors alive for exactly this
	// reason: a frame that reallocated its order buffer every time would spend
	// the measurement in the allocator, and — more to the point — would not be
	// what the renderer does. A benchmark that allocates where the subject does
	// not is measuring a different program.
	struct Buffers {
		std::vector<uint32_t> Visible;
		std::vector<DrawInstance> VisibleInstances;
		std::vector<uint32_t> DrawOrder;
		std::vector<uint32_t> SceneOrder;
		std::vector<DrawInstance> SceneInstances;
	};

	Buffers &BuffersOf() {
		static Buffers buffers;
		return buffers;
	}

	// Where the Nth of `views` viewports sits, spread around the slab.
	//
	// Distinct positions rather than one camera reused, because the blended sort
	// is keyed on distance from the eye and N identical eyes would let the
	// branch predictor and the cache carry every view after the first — which
	// would report split-screen as nearly free for the wrong reason.
	CFrame EyeAt(size_t view) {
		const float angle = 1.7f * static_cast<float>(view);
		return CFrame(Vector3(96.0f + 12.0f * angle, 4.0f + static_cast<float>(view), 96.0f - 9.0f * angle));
	}

	Camera LensOf() {
		Camera camera;
		camera.FieldOfViewRadians = 1.22f;
		camera.NearPlane = 0.1f;
		camera.FarPlane = 500.0f;
		return camera;
	}

	// One view's record, exactly as `Renderer::Render` performs it.
	//
	// The sequence and its order are copied from that function deliberately —
	// culled, then gathered, then ordered, then partitioned, then grouped —
	// because the order is load-bearing there: culling first means the sort runs
	// over the survivors rather than over the world.
	//
	// @param instances The draw list.
	// @param view      Which viewport.
	// @param bounds    Set to the whole list's union when `shareBounds` is
	//                  false, read and left alone when it is true.
	// @param shareBounds Whether the union bound was computed once for the frame
	//                  — the hoist — or is fused into this view's cull, which
	//                  is what the engine does today and what measured faster.
	void RecordView(std::span<const DrawInstance> instances, size_t view, AABB &bounds, bool shareBounds) {
		Buffers &buffers = BuffersOf();

		const CFrame eye = EyeAt(view);
		const float aspect = 1920.0f / 1080.0f;
		const Frustum frustum =
			Frustum::FromViewProjection(ResolveCamera(eye, LensOf(), aspect).ViewProjection);

		// **The one line the two models differ on.** `CullAndBound` walks the
		// list deriving each instance's world box, uses it for the frustum test
		// and unions it into `bounds`; `Cull` derives the same box and drops the
		// union. Every other line below is identical between the two rows.
		size_t visibleCount = 0;
		if (shareBounds) {
			visibleCount = Cull(instances, frustum, buffers.Visible);
		} else {
			visibleCount = CullAndBound(instances, frustum, buffers.Visible, bounds);
		}

		buffers.VisibleInstances.resize(visibleCount);
		for (size_t index = 0; index < visibleCount; index++) {
			buffers.VisibleInstances[index] = instances[buffers.Visible[index]];
		}

		const size_t opaqueCount = OrderForDrawing(buffers.VisibleInstances, eye.Position, buffers.DrawOrder);

		uint32_t surfaceInCamera = 0;
		if (opaqueCount > 0) {
			surfaceInCamera = static_cast<uint32_t>(PartitionSurfaces(
				buffers.VisibleInstances, std::span<uint32_t>(buffers.DrawOrder.data(), opaqueCount)
			));
		}
		const auto plainOpaque = static_cast<uint32_t>(opaqueCount) - surfaceInCamera;

		SurfaceRun cameraRuns[MAX_SURFACES];
		if (surfaceInCamera > 0) {
			GroupSurfaces(
				buffers.VisibleInstances,
				std::span<uint32_t>(buffers.DrawOrder.data() + plainOpaque, surfaceInCamera),
				plainOpaque,
				true,
				cameraRuns
			);
		}

		// The scene range: the whole draw list ordered from this eye, which is
		// what the shadow and surface passes submit. It is per view because the
		// eye is, and it is over the whole list because a caster outside the
		// frustum still shadows into it.
		buffers.SceneInstances.assign(instances.begin(), instances.end());
		const ScenePlan plan = OrderScene(buffers.SceneInstances, eye.Position, buffers.SceneOrder);

		Consume(visibleCount);
		Consume(plan.Opaque);
		Consume(cameraRuns[0].OpaqueCount);
	}

	// A whole frame: every view recorded, then the light fitted once.
	//
	// @param instances   The draw list.
	// @param views       How many viewports.
	// @param shareBounds Whether the union bound is computed once for the frame
	//                    rather than once per view.
	void RecordFrame(std::span<const DrawInstance> instances, size_t views, bool shareBounds) {
		AABB bounds;

		if (shareBounds) {
			// **The hoist, which measured slower.** One walk of the whole list
			// for the union every view's shadow fit needs — what a
			// `PerView = false` node would call. It loses because the per-view
			// `Cull` below still derives each instance's box to test it, so
			// this walk re-derives five thousand boxes to save the six
			// comparisons that unioning them costs. See the table above.
			bounds = engine::graph::BoundsOfAll(instances);
		}

		for (size_t view = 0; view < views; view++) {
			RecordView(instances, view, bounds, shareBounds);
		}

		Consume(FitDirectionalLight(bounds, SUN));
	}

	// A busy but not absurd world. Five thousand parts is a decorated room or a
	// small level — enough that the per-instance costs dominate the fixed ones,
	// and few enough that it is a scene somebody might actually build.
	constexpr size_t BUSY = 5'000;
}

using namespace frame_bench;

// **Every body below loops `Iterations` times itself**, which is the harness's
// actual contract — `BenchMain::Sample` calls the body *once* and divides the
// elapsed time by the declared count. A body that does its work once while
// declaring a thousand reports a thousandth of the truth, and reports it
// confidently. `graph/benchmarks/Cull.cpp` writes the loop out for this reason;
// `scene/benchmarks/Ordering.cpp` does not, because it is deliberately using
// the divisor to normalise per *instance* rather than per call. Both are legal
// and the rows are not comparable across the two — so the unit is stated here:
// **one iteration is one frame recorded.**

// --- what one view costs -----------------------------------------------------

BENCH("Resolve a camera and extract a frustum", 200'000) {
	for (int pass = 0; pass < 200'000; pass++) {
		const Frustum frustum = Frustum::FromViewProjection(
			ResolveCamera(EyeAt(static_cast<size_t>(pass % 4)), LensOf(), 1.78f).ViewProjection
		);
		Consume(frustum.Planes[0].Distance);
	}
}

BENCH("Record one view, 5000 instances", 200) {
	const std::vector<DrawInstance> &instances = SceneOf(BUSY);
	for (int pass = 0; pass < 200; pass++) {
		RecordFrame(instances, 1, false);
	}
}

// --- what the Nth view costs -------------------------------------------------
//
// The ladder v0.7's decision was never checked against. The interesting figure
// is not any row on its own but the gap between them divided by the extra
// views: that is what one more open viewport costs the CPU, and it is the
// number "expensive enough that its record is measurable" was waiting for.

BENCH("Record two views, 5000 instances", 100) {
	const std::vector<DrawInstance> &instances = SceneOf(BUSY);
	for (int pass = 0; pass < 100; pass++) {
		RecordFrame(instances, 2, false);
	}
}

BENCH("Record four views, 5000 instances", 50) {
	const std::vector<DrawInstance> &instances = SceneOf(BUSY);
	for (int pass = 0; pass < 50; pass++) {
		RecordFrame(instances, 4, false);
	}
}

// --- what hoisting the bound out of the cull is worth -------------------------
//
// The same four views, with the union bound computed once for the frame by
// `BoundsOfAll` instead of fused into each view's cull by `CullAndBound`. This
// is the obvious `PerView = false` candidate, and the row exists to find out
// whether it is one before anything is built on the assumption that it is.

BENCH("Record four views, bound hoisted, 5000 instances", 50) {
	const std::vector<DrawInstance> &instances = SceneOf(BUSY);
	for (int pass = 0; pass < 50; pass++) {
		RecordFrame(instances, 4, true);
	}
}

// --- how it scales with the world --------------------------------------------
//
// One view at three counts, so the shape of the growth is visible rather than
// assumed. The record is dominated by per-instance walks — cull, gather, order,
// partition — so this should be close to linear, and a row that is not is worth
// looking at.

BENCH("Record one view, 1000 instances", 1'000) {
	const std::vector<DrawInstance> &instances = SceneOf(1'000);
	for (int pass = 0; pass < 1'000; pass++) {
		RecordFrame(instances, 1, false);
	}
}

BENCH("Record one view, 20000 instances", 50) {
	const std::vector<DrawInstance> &instances = SceneOf(20'000);
	for (int pass = 0; pass < 50; pass++) {
		RecordFrame(instances, 1, false);
	}
}
