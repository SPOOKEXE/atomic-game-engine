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
		// Admission boundary that rejected the pipeline.
		PipelineAdmissionStage Stage = PipelineAdmissionStage::Validation;
		// Named declaration responsible for the refusal, when identified.
		core::Name Offender;
		// Human-readable detail for the rejected declaration.
		std::string Reason;
	};

	// A missing failure means the pipeline was accepted.
	struct PipelineAdmissionResult {
		// Empty when admission succeeded; otherwise describes the refusal.
		std::optional<PipelineFailure> Failure;

		// True when the pipeline passed admission.
		explicit operator bool() const {
			return !Failure.has_value();
		}
	};

	// Stable stage text shared by logs, Studio and control responses.
	const char *DescribePipelineAdmission(PipelineAdmissionStage stage);

	// Formats the same stage, offender and reason for text-only callers.
	std::string FormatPipelineFailure(const PipelineFailure &failure);

}
