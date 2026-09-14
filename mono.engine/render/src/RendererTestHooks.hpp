#pragma once

namespace engine::render::test_support {
	// Test-only fault injection for the native eight-target G-buffer probe.
	// It must be set before Initialise and reset after the fixture finishes.
	void SetForceGBufferPipelineFailure(bool enabled);
}
