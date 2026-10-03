#pragma once
#include <engine/imagegraph/FrameTime.hpp>

namespace studio::detail {
	// Each successful fixed tick is published before advancing. A pending retry
	// repeats the current clock; at most 4096 inclusive observations are admitted.
	struct ImageGraphExportPreparation {
		std::optional<engine::imagegraph::FrameTime> Current;
		engine::imagegraph::FrameTime Target{};
		bool Begin(
			std::optional<engine::imagegraph::FrameTime> previous,
			engine::imagegraph::FrameTime target,
			bool replay,
			std::string &failure
		) {
			using namespace engine::imagegraph;
			if (Current || !ValidFrameTime(target) || (previous && !ValidFrameTime(*previous))) {
				failure = "Export preparation frame is invalid or already active";
				return false;
			}
			Target = target;
			if (!replay || target.NegativeFrame ||
				(previous && !previous->NegativeFrame && previous->Tick == target.Tick &&
				 previous->Subframe <= target.Subframe)) {
				Current = target;
				return true;
			}
			const uint64_t first =
				previous && !previous->NegativeFrame && previous->Tick < target.Tick ? previous->Tick + 1 : 0;
			if (target.Tick - first >= 4096) {
				failure = "Export preparation seek exceeds the fixed-tick work budget";
				return false;
			}
			Current = first == target.Tick ? target : FrameTime{first};
			return true;
		}
		bool Complete() {
			if (!Current) return false;
			if (*Current == Target) return true;
			const uint64_t next = Current->Tick + 1;
			Current = next == Target.Tick ? Target : engine::imagegraph::FrameTime{next};
			return false;
		}
		void Clear() {
			*this = {};
		}
	};
}
