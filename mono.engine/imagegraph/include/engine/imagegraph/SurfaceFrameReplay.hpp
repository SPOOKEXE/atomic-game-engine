#pragma once

#include <engine/imagegraph/Document.hpp>

namespace engine::imagegraph {
	// Source processor caches retain their input per row and timeline frame.
	struct SurfaceFrameReplayEntry {
		std::string NodeId;
		uint64_t Frame = 0;
		size_t ProcessorRow = 0;
		Image Input;
		bool operator==(const SurfaceFrameReplayEntry &) const = default;
	};
	struct SurfaceFrameReplayState {
		std::vector<SurfaceFrameReplayEntry> Entries;
		uint64_t Tick = 0;
		bool Initialized = false;
		bool operator==(const SurfaceFrameReplayState &) const = default;
	};
	uint64_t RetainedSurfaceFrameReplayBytes(const SurfaceFrameReplayState &state);
	uint64_t RetainedSurfaceFrameEntryBytes(const SurfaceFrameReplayEntry &entry);
	Status ValidateSurfaceFrameReplay(
		const SurfaceFrameReplayState &state, uint64_t maximumBytes, Diagnostic &diagnostic
	);
}
