#pragma once

#include <engine/imagegraph/Document.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine::imagegraph {
	// Fixed source camera observations derived from project settings and resolved links.
	struct SourceCameraEvaluationPolicy {
		uint32_t ProjectWidth = 32, ProjectHeight = 32;
		int64_t ProjectColorDepth = 1, ProjectShader3D = 0;
		bool DimensionLinked = false;
		std::optional<SurfaceFormat> InheritedSurfaceFormat = SurfaceFormat::RGBA8Unorm;
		bool operator==(const SourceCameraEvaluationPolicy &) const = default;
	};
	constexpr bool ValidSourceCameraEvaluationPolicy(const SourceCameraEvaluationPolicy &policy) {
		return policy.ProjectWidth > 0 && policy.ProjectHeight > 0 &&
			   policy.ProjectWidth <= Limits::MaximumDimension &&
			   policy.ProjectHeight <= Limits::MaximumDimension && policy.ProjectColorDepth >= 0 &&
			   policy.ProjectColorDepth <= 6 && policy.ProjectShader3D >= 0 && policy.ProjectShader3D <= 1 &&
			   (!policy.InheritedSurfaceFormat ||
				uint8_t(*policy.InheritedSurfaceFormat) <= uint8_t(SurfaceFormat::R32Float));
	}
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
		std::optional<SourceCameraEvaluationPolicy> CameraPolicy{};
		std::optional<uint32_t> CameraRow{};
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
		// Resolved source interpolation policy; 1 Pixel disables device filtering.
		int64_t Interpolation = 1;
		std::optional<SourceCameraEvaluationPolicy> CameraPolicy{};
		std::optional<uint32_t> CameraRow{};
	};
	// Owned receipt payload and capacities, excluding allocator bookkeeping.
	std::optional<uint64_t> HostCaptureRetainedPayloadBytes(const HostNodeCapture &);
	// Copies already resolved receipt metadata within an explicit budget; no producers execute.
	Status PrepareResolvedHostCapture(
		const HostNodeInvocation &,
		uint64_t maximumBytes,
		HostNodeCapture &,
		uint64_t &retainedBytes,
		Diagnostic &
	);
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
