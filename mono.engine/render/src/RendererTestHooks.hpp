#pragma once

#include <optional>

namespace engine::render::test_support {
	// Test-only fault injection for the native eight-target G-buffer probe.
	// It must be set before Initialise and reset after the fixture finishes.
	void SetForceGBufferPipelineFailure(bool enabled);

	// Forces only the next analytical-field buffer reservation path to refuse.
	// It proves a larger requested preset retains the last working device field.
	void SetForceGpuParticleFieldAllocationFailure(bool enabled);

	// Stops a batch before the selected view group records. The frame still owns
	// a real command buffer, so cleanup follows the same partial-submit path.
	// Passing no group disables the hook; release builds always leave it disabled.
	void SetFrameBatchFailureBeforeGroupForTests(std::optional<size_t> group);

	// Simulates a failed acquisition before the backend is called.
	void SetFrameBatchAcquisitionFailureForTests(bool enabled);

	// Cancels the next batch command at the submission boundary.
	void SetFrameBatchSubmissionFailureForTests(bool enabled);
	bool ConsumeFrameBatchSubmissionFailureForTests();
}
