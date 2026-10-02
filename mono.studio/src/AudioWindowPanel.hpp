#pragma once
#include <engine/imagegraph/AudioWindowPresentation.hpp>
#include <engine/imagegraph/FrameTime.hpp>
namespace studio {
	// One selected inspector retains the last good geometry and exact request identity.
	class AudioWindowPanel {
	  public:
		bool Update(
			const engine::imagegraph::Document &,
			std::string_view nodeId,
			const engine::imagegraph::EvaluationRequest &,
			uint64_t documentRevision,
			uint64_t inputRevision,
			engine::imagegraph::Diagnostic &,
			uint64_t byteBudget = engine::imagegraph::Limits::MaximumEvaluationBytes
		);
		void Draw(float height = 80) const;
		const engine::imagegraph::AudioWindowPresentation *Current() const {
			return Valid ? &Observation : nullptr;
		}

	  private:
		engine::imagegraph::Plan Compiled;
		engine::imagegraph::AudioWindowPresentation Observation;
		std::string Target;
		uint64_t PlanRevision = 0, AttemptDocument = 0, AttemptInput = 0, AttemptBudget = 0;
		uint32_t AttemptMaximumImageDimension = 0;
		engine::imagegraph::FrameTime AttemptFrame;
		engine::imagegraph::Diagnostic LastError;
		bool HavePlan = false, Attempted = false, Valid = false;
	};
}
