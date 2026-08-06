// What the value types and the deterministic generator cost in bulk.
//
// **`Random` is the one to read first, and it is not cheap.** Every function on
// it is the first 32 bits of SHA-256 over the `(index, salt)` pair — a
// specified algorithm rather than a constant nobody can check, which is exactly
// why it was chosen, but a full hash compression round is three orders of
// magnitude more work than the integer mixer it replaced. That is a defensible
// trade for a spawn path called once per entity and an indefensible one for
// something called per frame per entity, and the only way to know which side of
// the line a call site is on is to have the figure. It is here.
//
// The `CFrame` and `AABB` rows are the arithmetic every part in a scene goes
// through on its way to a draw call. They are bulk on purpose: one transform
// composed in isolation measures the pipeline and the call, and a hundred
// thousand of them over an array measures what a frame actually does.

#include <engine/core/Random.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/CFrame.hpp>
#include <engine/core/types/Ray.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/testing/Bench.hpp>

#include <cstdint>
#include <vector>

TEST_SUITE_ID("engine.core.bench.values")

using engine::core::AABB;
using engine::core::CFrame;
using engine::core::Random;
using engine::core::Ray;
using engine::core::Vector3;
using engine::testing::Consume;

namespace values_bench {

	// The population every bulk row walks.
	//
	// 100k is a world's worth of parts — the same order the `ecs` iteration
	// suite uses — so a figure here divides into a per-part cost that can be
	// compared against the `Each` rows directly.
	constexpr size_t COUNT = 100'000;

	// Transforms scattered through a room-shaped volume, built once.
	//
	// Deterministic through `Random`, which is indexed rather than streamed, so
	// two runs measure the same data and a difference between them is the code.
	const std::vector<CFrame> &Frames() {
		static const std::vector<CFrame> frames = [] {
			std::vector<CFrame> built;
			built.reserve(COUNT);
			for (uint32_t index = 0; index < COUNT; index++) {
				const Vector3 position(
					Random::Range(index, 3, -128.0f, 128.0f),
					Random::Range(index, 5, -8.0f, 8.0f),
					Random::Range(index, 7, -128.0f, 128.0f)
				);
				built.push_back(CFrame::LookAt(
					position,
					position + Vector3(
								   Random::Range(index, 11, -1.0f, 1.0f),
								   Random::Range(index, 13, -1.0f, 1.0f),
								   Random::Range(index, 17, -1.0f, 1.0f)
							   )
				));
			}
			return built;
		}();
		return frames;
	}

	// Boxes over the same scatter, for the overlap rows.
	const std::vector<AABB> &Boxes() {
		static const std::vector<AABB> boxes = [] {
			std::vector<AABB> built;
			built.reserve(COUNT);
			for (const CFrame &frame : Frames()) {
				const Vector3 extent(1.0f, 1.0f, 1.0f);
				built.push_back(AABB{frame.Position - extent, frame.Position + extent});
			}
			return built;
		}();
		return boxes;
	}

	// Points to transform, so the transform rows are not also measuring a
	// generator.
	const std::vector<Vector3> &Points() {
		static const std::vector<Vector3> points = [] {
			std::vector<Vector3> built;
			built.reserve(COUNT);
			for (uint32_t index = 0; index < COUNT; index++) {
				built.emplace_back(
					Random::Range(index, 19, -1.0f, 1.0f),
					Random::Range(index, 23, -1.0f, 1.0f),
					Random::Range(index, 29, -1.0f, 1.0f)
				);
			}
			return built;
		}();
		return points;
	}
}

using namespace values_bench;

// --- the deterministic generator ----------------------------------------------

BENCH("Random::Bits · 100k", COUNT) {
	uint32_t mixed = 0;
	for (uint32_t index = 0; index < COUNT; index++) {
		mixed ^= Random::Bits(index, 3);
	}
	Consume(mixed);
}

BENCH("Random::Float · 100k", COUNT) {
	float total = 0.0f;
	for (uint32_t index = 0; index < COUNT; index++) {
		total += Random::Float(index, 3);
	}
	Consume(total);
}

BENCH("Random::Range · 100k", COUNT) {
	float total = 0.0f;
	for (uint32_t index = 0; index < COUNT; index++) {
		total += Random::Range(index, 3, -128.0f, 128.0f);
	}
	Consume(total);
}

BENCH("Random · one Vector3 each, 100k", COUNT) {
	// **The shape a spawn path actually calls: three salts for one position.**
	// Three SHA-256 compressions per entity, and this row is what that is worth
	// per entity spawned. Read it against `Each · 100k entities` in the `ecs`
	// suite: if generating a position costs more than iterating every entity in
	// the world does, a spawn loop that generates per frame is the bug and this
	// is where it is visible.
	float total = 0.0f;
	for (uint32_t index = 0; index < COUNT; index++) {
		const Vector3 position(
			Random::Range(index, 3, -128.0f, 128.0f),
			Random::Range(index, 5, -8.0f, 8.0f),
			Random::Range(index, 7, -128.0f, 128.0f)
		);
		total += position.X + position.Y + position.Z;
	}
	Consume(total);
}

BENCH("control · xorshift32, 100k", COUNT) {
	// The integer mixer `Random` replaced, at the same call count. **Not a
	// proposal to go back** — this one is neither specified nor portable, which
	// is the whole reason it went — but the ratio is what determinism-by-SHA-256
	// costs, and a number nobody has is a number nobody can weigh.
	uint32_t state = 0x9E37'79B9u;
	uint32_t mixed = 0;
	for (uint32_t index = 0; index < COUNT; index++) {
		state ^= index;
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		mixed ^= state;
	}
	Consume(mixed);
}

// --- transforms ---------------------------------------------------------------

BENCH("CFrame · compose 100k", COUNT) {
	const std::vector<CFrame> &frames = Frames();
	const CFrame offset(Vector3(0.0f, 1.0f, 0.0f));
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += (frames[index] * offset).Position.X;
	}
	Consume(total);
}

BENCH("CFrame · PointToWorldSpace 100k", COUNT) {
	// The per-vertex, per-part operation on the way to a draw call. If this ever
	// becomes the frame's cost centre, the answer is a batched form rather than
	// a faster scalar one, and this row is where that argument starts.
	const std::vector<CFrame> &frames = Frames();
	const std::vector<Vector3> &points = Points();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += frames[index].PointToWorldSpace(points[index]).X;
	}
	Consume(total);
}

BENCH("CFrame · Inverse 100k", COUNT) {
	const std::vector<CFrame> &frames = Frames();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += frames[index].Inverse().Position.X;
	}
	Consume(total);
}

BENCH("CFrame · LookAt 100k", COUNT) {
	// Builds a basis from two points, which is a cross product, two
	// normalisations and a quaternion conversion — much the dearest thing on
	// `CFrame`. A camera does it once a frame and a billboarding system does it
	// per part, and those are very different bills.
	const std::vector<CFrame> &frames = Frames();
	const std::vector<Vector3> &points = Points();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += CFrame::LookAt(frames[index].Position, frames[index].Position + points[index]).Position.X;
	}
	Consume(total);
}

// --- boxes --------------------------------------------------------------------

BENCH("AABB · 100k pairwise overlap tests", COUNT) {
	// The narrow test a broad phase runs after the grid has narrowed things
	// down. `spatial`'s suite measures the grid; this measures what the grid
	// hands off to, and the two together are the whole broad-phase bill.
	const std::vector<AABB> &boxes = Boxes();
	uint32_t hits = 0;
	for (size_t index = 0; index < COUNT; index++) {
		hits += boxes[index].Overlaps(boxes[(index + 1) % COUNT]) ? 1u : 0u;
	}
	Consume(hits);
}

BENCH("AABB · 100k point containment tests", COUNT) {
	const std::vector<AABB> &boxes = Boxes();
	const std::vector<Vector3> &points = Points();
	uint32_t hits = 0;
	for (size_t index = 0; index < COUNT; index++) {
		hits += boxes[index].Contains(points[index]) ? 1u : 0u;
	}
	Consume(hits);
}

BENCH("AABB · 100k unions", COUNT) {
	// What building a bounding volume over a scene costs, which is the loop a
	// culling structure runs every time something moves.
	const std::vector<AABB> &boxes = Boxes();
	AABB total = boxes[0];
	for (size_t index = 0; index < COUNT; index++) {
		total = total.Union(boxes[index]);
	}
	Consume(total.Minimum.X);
}

// --- vectors ------------------------------------------------------------------

BENCH("Vector3 · 100k normalise", COUNT) {
	const std::vector<Vector3> &points = Points();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += points[index].Unit().X;
	}
	Consume(total);
}

BENCH("Vector3 · 100k dot", COUNT) {
	const std::vector<Vector3> &points = Points();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += points[index].Dot(points[(index + 1) % COUNT]);
	}
	Consume(total);
}

BENCH("Vector3 · 100k cross", COUNT) {
	const std::vector<Vector3> &points = Points();
	float total = 0.0f;
	for (size_t index = 0; index < COUNT; index++) {
		total += points[index].Cross(points[(index + 1) % COUNT]).X;
	}
	Consume(total);
}
