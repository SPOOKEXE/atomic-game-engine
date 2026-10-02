#pragma once
#include <engine/imagegraph/WavTimelinePresentation.hpp>
namespace studio {
	// One visible inspector owns one bounded waveform; hidden inspectors perform no observation work.
	class WavTimelinePanel {
	  public:
		// Resolves this visible node once per exact request identity; reuses unchanged point geometry.
		bool Update(
			const engine::imagegraph::Document &document,
			std::string_view nodeId,
			const engine::imagegraph::EvaluationRequest &request,
			double fps,
			uint64_t documentRevision,
			uint64_t inputRevision,
			engine::imagegraph::Diagnostic &diagnostic,
			uint64_t byteBudget = engine::imagegraph::Limits::MaximumEvaluationBytes
		);
		// Draws the current observation only, with no graph work or retained buffer copying.
		void Draw(float height = 80) const;
		// Returns null after any failed refresh so the inspector cannot show stale data as current.
		const engine::imagegraph::WavTimelinePresentation *Current() const {
			return Valid ? &Geometry : nullptr;
		}

	  private:
		engine::imagegraph::Plan Compiled;
		engine::imagegraph::WavTimelinePresentation Geometry;
		std::string GeometryPath;
		std::string Target;
		uint64_t PlanRevision = 0, GeometryInputRevision = 0;
		double GeometryFps = 0;
		bool Valid = false, HaveGeometry = false, HavePlan = false, Attempted = false;
		uint64_t AttemptDocument = 0, AttemptInput = 0;
		double AttemptFps = 0;
		uint64_t AttemptBudget = 0;
		engine::imagegraph::FrameTime AttemptFrame;
		engine::imagegraph::Diagnostic LastError;
	};
}
