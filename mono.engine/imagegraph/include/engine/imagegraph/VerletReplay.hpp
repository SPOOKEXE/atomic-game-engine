#pragma once

// grug keep source verlet state in caller-owned snapshots. no clock or ambient random draws.

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace engine::imagegraph {
	struct VerletReplayState {
		VerletMesh Mesh;
		uint64_t Tick = 0, AuthoringRevision = 0;
		bool Initialized = false;
		bool operator==(const VerletReplayState &) const = default;
	};
	struct VerletStepSettings {
		uint32_t Substeps = 8;
		Vector2 Gravity{0, .5};
		// grug simple source node uses gravity/substeps. inline group uses gravity/substeps/10.
		bool Simple = false;
		uint32_t Wall = 0;
		Vector2 Dimension{1, 1};
		uint64_t MaximumBytes = Limits::MaximumEvaluationBytes;
		uint64_t MaximumWork = 1'048'576;
	};
	// grug validate and copy before replacing snapshot. failed reset leaves existing state alone.
	[[nodiscard]] Status ResetVerletReplay(
		std::span<const VerletPoint> points,
		std::span<const VerletEdge> edges,
		uint64_t tick,
		uint64_t authoringRevision,
		uint64_t maximumBytes,
		VerletReplayState &state,
		Diagnostic &diagnostic
	);
	// grug accepts only next fixed tick and same authored revision. seek requires reset and replay.
	[[nodiscard]] Status StepVerletReplay(
		const VerletReplayState &previous,
		uint64_t tick,
		uint64_t authoringRevision,
		const VerletStepSettings &settings,
		VerletReplayState &next,
		Diagnostic &diagnostic
	);
}
