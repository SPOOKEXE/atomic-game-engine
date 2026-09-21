#pragma once

#include <engine/render/Renderer.hpp>

#include <cstdint>

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

	  private:
		Renderer &Render;
	};
}
