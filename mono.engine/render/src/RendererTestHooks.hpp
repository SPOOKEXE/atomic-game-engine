#pragma once

#include <optional>

namespace engine::render::test_support {
	// Test-only fault injection for the native eight-target G-buffer probe.
	// It must be set before Initialise and reset after the fixture finishes.
	void SetForceGBufferPipelineFailure(bool enabled);

	// Stops a batch before the selected view group records. The frame still owns
	// a real command buffer, so cleanup follows the same partial-submit path.
	// Passing no group disables the hook; release builds always leave it disabled.
	void SetFrameBatchFailureBeforeGroupForTests(std::optional<size_t> group);
}
