#pragma once
#include "ImageGraphHost.hpp"

#include <engine/imagegraph/PendingHostObservations.hpp>

namespace studio::detail {
	// A preview retries one captured frame. Successful capabilities remain owned
	// across pending processor rows; changing its generation cancels the receipts.
	struct ImageGraphPreviewObservations {
		engine::imagegraph::PendingHostObservations Receipts;
		std::optional<engine::imagegraph::FrameTime> Frame;
		void Clear() {
			Receipts.Clear();
			Frame.reset();
		}
		void Begin(engine::imagegraph::FrameTime frame) {
			if (Frame != frame) {
				Clear();
				Frame = frame;
			}
			Receipts.BeginAttempt();
		}
	};
	struct ImageGraphPreviewProvider final : engine::imagegraph::HostNodeProvider {
		ImageGraphHost &Host;
		ImageGraphPreviewObservations &Observations;
		const engine::imagegraph::EvaluationRequest &Request;
		ImageGraphPreviewProvider(
			ImageGraphHost &host,
			ImageGraphPreviewObservations &observations,
			const engine::imagegraph::EvaluationRequest &request
		)
			: Host(host), Observations(observations), Request(request) {}
		bool Capture(
			const engine::imagegraph::HostNodeInvocation &invocation,
			engine::imagegraph::HostNodeCapture &output,
			std::string &failure
		) override {
			return Observations.Receipts.CaptureSequenced(invocation, Host, output, failure);
		}
		bool PcxMessages(
			std::string_view node,
			std::span<const engine::imagegraph::PcxMessage> messages,
			std::string &failure
		) override {
			const uint64_t maximum = engine::imagegraph::Limits::MaximumEvaluationBytes;
			if (Host.RetainedBytes >= maximum || Host.LuaReceipts.Bytes >= maximum - Host.RetainedBytes) {
				failure = "Retained preview observations leave no message budget";
				return false;
			}
			return Observations.Receipts.ForwardMessages(
				Request, node, messages, Host, maximum - Host.RetainedBytes - Host.LuaReceipts.Bytes, failure
			);
		}
	};
}
