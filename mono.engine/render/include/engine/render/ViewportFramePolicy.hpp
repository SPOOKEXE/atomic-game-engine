#pragma once

// The retained-target refresh decision for one ViewportFrame.
//
// This stays free of renderer and SDL state so the exact cache contract can be
// tested without a device. The caller supplies a signature containing every
// pixel-affecting mini-scene input and commits only after recording a frame.

#include <engine/gui/Enums.hpp>

#include <algorithm>
#include <cstdint>

namespace engine::render {

	// Cached input state for one viewport target.
	struct ViewportFrameCache {
		uint64_t Signature = 0;			   // Last rendered pixel signature.
		uint64_t RenderGeneration = 0;	   // Generation of the last render.
		uint32_t InvalidationRevision = 0; // Last manual invalidation.
		bool Ready = false;				   // Whether this cache contains pixels.

		// Returns whether the selected update policy requires a render.
		bool NeedsRender(
			gui::ViewportUpdateMode mode,
			uint32_t everyFrames,
			uint32_t invalidationRevision,
			uint64_t signature,
			uint64_t generation
		) const {
			if (!Ready || Signature != signature) return true;
			switch (mode) {
			case gui::ViewportUpdateMode::OnChange:
				return false;
			case gui::ViewportUpdateMode::EveryFrame:
				return true;
			case gui::ViewportUpdateMode::FixedRate:
				return generation - RenderGeneration >= std::max<uint32_t>(everyFrames, 1);
			case gui::ViewportUpdateMode::Manual:
				return InvalidationRevision != invalidationRevision;
			}
			return true;
		}

		// Records a successfully rendered target.
		void Commit(uint64_t signature, uint64_t generation, uint32_t invalidationRevision) {
			Signature = signature;
			RenderGeneration = generation;
			InvalidationRevision = invalidationRevision;
			Ready = true;
		}
	};

	// The entity whose pixels a retained renderer slot currently contains.
	// Entity cache state alone cannot prove this: another viewport can use the
	// same slot while the first is hidden, then leave a valid but wrong image.
	struct ViewportFrameTargetOwner {
		uint64_t Instance = 0;		  // Instance whose pixels occupy the target.
		uint64_t WriteGeneration = 0; // Generation that wrote those pixels.

		// Returns whether the target contains this instance.
		bool Owns(uint64_t instance) const {
			return instance != 0 && Instance == instance;
		}

		// Records the instance and generation that wrote this target.
		void Commit(uint64_t instance, uint64_t generation) {
			Instance = instance;
			WriteGeneration = generation;
		}
	};
}
