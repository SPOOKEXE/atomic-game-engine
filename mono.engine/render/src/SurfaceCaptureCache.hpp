#pragma once

// The retained result of surface-capture planning for one viewport.
//
// Planning depends only on its camera, pane and portal descriptions, dimensions
// and capture budget. The caller fingerprints those inputs, so an unchanged
// view keeps its previous plan instead of traversing the same capture tree
// every frame.

#include <cstdint>

namespace engine::render {

	struct SurfaceCaptureCache {
		bool Ready = false;
		bool BudgetExceeded = false;
		uint64_t Pixels = 0;
		uint64_t Signature = 0;

		bool NeedsRefresh(uint64_t signature) const {
			return !Ready || Signature != signature;
		}

		void Commit(uint64_t signature, uint64_t pixels, bool budgetExceeded) {
			Ready = true;
			Signature = signature;
			Pixels = pixels;
			BudgetExceeded = budgetExceeded;
		}
	};
}
