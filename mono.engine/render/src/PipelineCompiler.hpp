#pragma once

// Private pipeline-compiler diagnostics shared by renderer admission paths.

#include "RendererState.hpp"

namespace engine::render {
	const char *DescribePipelineAdmission(PipelineAdmissionStage stage);
}
