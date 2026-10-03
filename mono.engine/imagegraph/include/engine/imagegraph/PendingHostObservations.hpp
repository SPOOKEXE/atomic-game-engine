#pragma once
#include <engine/imagegraph/HostCapture.hpp>
#include <engine/imagegraph/PcxExpression.hpp>

#include <array>
#include <optional>

namespace engine::imagegraph {
	// One pending evaluation owns exact capability observations, so retrying its
	// immutable frame does not repeat upstream host side effects.
	struct PendingHostMessageReceipt {
		std::string Node;
		std::vector<PcxMessage> Messages;
	};
	struct PendingHostInvocationContext {
		std::optional<TimelineSettings> Timeline;
		std::optional<SurfaceFormat> OutputFormat;
		int64_t Interpolation = 1;
	};
	struct PendingHostObservations {
		std::vector<HostNodeCapture> Captures;
		uint64_t Bytes = 0, Tick = 0, Seed = 0;
		double Subframe = 0;
		bool NegativeFrame = false, Active = false;
		std::vector<PendingHostMessageReceipt> MessageReceipts;
		size_t MessageCursor = 0, CaptureCursor = 0;
		std::array<size_t, 64> CaptureSequences{};
		std::array<PendingHostInvocationContext, 64> CaptureContexts{};
		std::optional<PendingHostInvocationContext> PendingContext;
		std::optional<HostNodeCapture> PendingInput;
		size_t PendingSequence = 0;
		// Reset once before each immutable-frame evaluation retry, never between node callbacks.
		void BeginAttempt() noexcept;
		// Admission charge includes receipt clone footprints and all fixed/container backing.
		// It is conservative owned-payload accounting, not a heap measurement.
		std::optional<uint64_t> RetainedPayloadBytes() const;
		bool ForwardMessages(
			const EvaluationRequest &,
			std::string_view node,
			std::span<const PcxMessage>,
			HostNodeProvider &,
			uint64_t maximumBytes,
			std::string &failure
		);
		void Clear();
		bool Capture(
			const HostNodeInvocation &,
			HostNodeProvider &,
			HostNodeCapture &,
			std::string &,
			size_t sequence = 0
		);
		// Evaluation-local callback ordinal binds distinct processor rows without serializing an ID.
		bool
		CaptureSequenced(const HostNodeInvocation &, HostNodeProvider &, HostNodeCapture &, std::string &);
	};
}
