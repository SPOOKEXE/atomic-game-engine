#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::imagegraph {
	struct HostCapturedImage {
		std::string Port;
		Image Data;
	};
	struct HostCapturedImageArray {
		std::string Port;
		std::vector<Image> Frames;
	};
	struct HostImageBinding {
		std::string Port;
		uint64_t Hash = 0;
	};
	enum class HostCaptureState : uint8_t { Recorded, Refused, Failed };
	// Immutable host observations bind both the authored node and its resolved controls.
	// Recorded inputs replay without granting filesystem, process, network or device access.
	struct HostNodeCapture {
		Node Authored;
		uint64_t Tick = 0;
		double Subframe = 0;
		bool NegativeFrame = false;
		std::vector<AuthoredValue> Inputs;
		std::vector<HostImageBinding> InputImages;
		std::vector<AuthoredValue> Outputs;
		std::vector<HostCapturedImage> Images;
		std::vector<HostCapturedImageArray> ImageArrays;
		HostCaptureState State = HostCaptureState::Recorded;
		std::string Failure;
	};
	// Resolve controls before host work, without executing the selected capability node.
	// Failed preparation leaves the previous capture unchanged.
	Status PrepareHostCapture(
		const Document &document,
		const Plan &plan,
		std::string_view nodeId,
		const EvaluationRequest &request,
		HostNodeCapture &capture,
		Diagnostic &diagnostic
	);

	struct HostResolvedImage {
		std::string_view Port;
		const Image *Data = nullptr;
	};
	struct HostNodeInvocation {
		const Node &Authored;
		const EvaluationRequest &Request;
		std::span<const AuthoredValue> Inputs;
		std::span<const HostResolvedImage> Images;
		uint64_t MaximumOperationBytes = 0;
		const TimelineSettings *Timeline = nullptr;
		std::optional<SurfaceFormat> OutputFormat = SurfaceFormat::RGBA8Unorm;
	};
	// Process-local host capability. Durable graphs and copied recordings contain no provider pointer.
	struct PcxMessage;
	class HostNodeProvider {
	  public:
		virtual ~HostNodeProvider() = default;
		virtual bool PcxMessages(std::string_view, std::span<const PcxMessage>, std::string &failure) {
			failure = "PCX notifications require an explicit host capability";
			return false;
		}
		virtual bool
		Capture(const HostNodeInvocation &invocation, HostNodeCapture &output, std::string &failure) = 0;
	};

}
