#pragma once
#include <engine/imagegraphexport/GraphAuthoredExport.hpp>

#include <memory>

namespace engine::imagegraphexport {
	// Process-local generations attest immutable document, observation, grant and capture-owner identity.
	struct GraphExportGeneration {
		uint64_t Authoring = 0, Inputs = 0, Observations = 0, Grants = 0, Owner = 0;
		bool operator==(const GraphExportGeneration &) const = default;
	};
	enum class GraphExportProgress { Pending, Progress, Complete, Failed };
	class GraphExportSessionHost : public engine::imagegraph::HostNodeProvider {
	  public:
		// Reset the pending observation before each Resume. Failure text is never used as a pending signal.
		virtual bool Pending() const noexcept = 0;
		virtual void Cancel() noexcept = 0;
	};
	// Collects authored Animation/Sequence frames without a synchronous device wait.
	// Each Resume borrows providers and observations only until it returns. Caller generations attest
	// that these observations and grants belong to the frozen intent, including audio and device owner.
	class GraphExportSession {
		struct State;
		std::unique_ptr<State> Inside;

	  public:
		GraphExportSession();
		~GraphExportSession();
		GraphExportSession(const GraphExportSession &) = delete;
		GraphExportSession &operator=(const GraphExportSession &) = delete;
		bool BeginAuthored(
			const engine::imagegraph::Document &,
			const engine::imagegraph::EvaluationSnapshot &,
			const engine::imagegraph::EvaluationRequest &,
			const GraphExportSettings &,
			std::string_view nodeId,
			GraphExportGeneration,
			std::string &failure
		);
		GraphExportProgress Resume(
			const engine::imagegraph::EvaluationRequest &,
			GraphExportGeneration,
			GraphExportSessionHost &,
			std::string &failure
		);
		std::optional<engine::imagegraph::FrameTime> NextFrame() const;
		std::span<const std::filesystem::path> RetainedDirectories() const;
		// Conservative capture/collection owned-data reservation, not measured heap use. Existing
		// compiler, CPU evaluation and final encoder workspaces have independent operation caps.
		static uint64_t MaximumPayloadReservationBytes() noexcept;
		uint64_t PayloadReservationBytes() const noexcept;
		// Call before destruction/replacement when device work may be pending. Destruction removes
		// owned files, but cannot cancel a caller's provider without retaining a borrowed pointer.
		void Cancel(GraphExportSessionHost &) noexcept;
	};
}
