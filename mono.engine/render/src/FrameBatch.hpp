#pragma once

#include <engine/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace engine::render {
	enum class FrameBatchOutcome : uint8_t {
		SkippedBeforeAcquisition,
		Submitted,
		SubmittedAfterViewFailure,
		Aborted,
	};

	struct FrameBatchResult {
		FrameResult Frame;
		FrameBatchOutcome Outcome = FrameBatchOutcome::SkippedBeforeAcquisition;
	};

	struct FrameBatchTestSnapshot {
		bool Active = false;
		bool CommandOwned = false;
		bool VisibilityValid = false;
		size_t PendingHistoryWrites = 0;
		size_t ReadyHistoryTargets = 0;
		size_t SubmissionAttempts = 0;
		uint64_t HistoryFingerprint = 0;
	};

	// Owns one render call from view grouping through command submission and cleanup.
	class FrameBatch {
	  public:
		explicit FrameBatch(Renderer &renderer) : Render(renderer) {}

		FrameBatchResult
		Run(std::span<const View> views,
			OverlayImage &overlay,
			FrameOverlayHook *gameInterfaceHook,
			bool present,
			FrameOverlayHook *hostOverlayHook);

		static FrameBatchTestSnapshot SnapshotForTests(const Renderer &renderer);
		static std::optional<uint32_t>
		ParticleCellForTests(const Renderer &renderer, uint64_t world, core::Name worldName, uint32_t row);

	  private:
		Renderer &Render;
	};
}
