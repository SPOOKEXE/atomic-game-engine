#pragma once

// The reason a render pipeline was refused before installation.

#include <engine/core/Name.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace engine::render {

	// The boundary where pipeline admission stopped.
	enum class PipelineAdmissionStage : uint8_t { Graph, Schedule, Backend, Validation, Capability };

	// The declaration and reason associated with one rejected admission.
	struct PipelineFailure {
		PipelineAdmissionStage Stage = PipelineAdmissionStage::Validation;
		core::Name Offender;
		std::string Reason;
	};

	// A missing failure means the pipeline was accepted.
	struct PipelineAdmissionResult {
		std::optional<PipelineFailure> Failure;

		explicit operator bool() const {
			return !Failure.has_value();
		}
	};

	// Stable stage text shared by logs, Studio and control responses.
	const char *DescribePipelineAdmission(PipelineAdmissionStage stage);

	// Formats the same stage, offender and reason for text-only callers.
	std::string FormatPipelineFailure(const PipelineFailure &failure);

}
