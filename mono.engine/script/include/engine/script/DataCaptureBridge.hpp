#pragma once

// A product-installed bridge from the VM-neutral script surface to an L12
// renderer. Script owns no renderer object or ticket state.
// @tier L9 · shared

#include <engine/script/Codec.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
	struct DataCaptureBridgeRequest {
		std::string InstanceId;
		std::string SnapshotId;
		std::string Pipeline;
		std::string CaptureNode;
		// `current_view` keeps the host's selected view. Any other value is a
		// stable authored DataFactoryId for one Camera and Transform in the world.
		std::string CameraId = "current_view";
		uint64_t ViewSlot = 0;
		std::vector<std::string> Channels;
		std::string TemporalHistory;
		// "lossless" retains renderer readback bytes. "training_compact" is an
		// explicit storage transform with per-plane encoding metadata.
		std::string StorageProfile = "lossless";
		// `gaussian` applies a deterministic bounded perturbation to copied
		// rgb_linear_hdr colour lanes only. It never changes alpha or a label plane.
		std::string NoiseMode = "none";
		uint64_t NoiseSeed = 0;
		double NoiseSigma = 0.0;
		// The renderer copies a scene sidecar only after its retained-snapshot
		// barrier succeeds. This flag asks for that bounded copied observation.
		bool IncludeSceneData = false;
	};

	// A scene observation retained with one capture ticket. The lifecycle values
	// identify the exact paused state from which `Scene` was copied.
	struct DataCaptureBridgeSceneSidecar {
		std::string SnapshotId;
		uint64_t Tick = 0;
		uint64_t WorldEpoch = 0;
		uint64_t WorldVersion = 0;
		std::string StorageProfile = "lossless";
		ScriptValue Scene;
	};

	struct DataCaptureBridgeAmbientOcclusion {
		std::string SourceState;
		std::optional<uint64_t> ProducerFrame;
		std::optional<bool> Enabled;
		std::optional<uint32_t> SampleCount;
		std::optional<double> RadiusWorldUnits;
		std::optional<std::string> Denoiser;
		std::optional<std::string> TemporalHistory;
		std::optional<double> BackgroundValue;
		std::optional<std::string> BackgroundClassification;
	};

	// Describes the copied RGB perturbation. The native renderer bytes remain
	// addressable through the source descriptor on the same plane.
	struct DataCaptureBridgeNoise {
		std::string Mode;
		std::string Algorithm;
		uint64_t Seed = 0;
		double Sigma = 0.0;
		std::string SigmaQuantization;
		uint64_t EffectiveSigmaQ24 = 0;
		double EffectiveSigma = 0.0;
		std::string SeedStatePolicy;
		std::string Order;
		std::string ClampPolicy;
		std::string AlphaPolicy;
		std::string ValueClassification;
		std::optional<double> MaximumAbsoluteError;
	};

	struct DataCaptureBridgePlane {
		std::string Channel;
		std::string Status;
		// An opaque capture-local identifier. It is accepted only with its
		// owning ticket and does not name a renderer graph resource.
		std::string Resource;
		std::string SourceResource;
		std::string HashAlgorithm;
		std::string Hash;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
		size_t ByteSize = 0;
		std::string Scalar;
		// SourceScalar is the source value representation. Scalar and Encoding
		// describe the retained bytes returned by ReadPlane.
		std::string SourceScalar;
		std::string SourceHash;
		uint32_t SourceWidth = 0;
		uint32_t SourceHeight = 0;
		uint32_t SourceRowStride = 0;
		size_t SourceByteSize = 0;
		std::string SourceEncoding;
		std::string SourceColourSpace;
		std::string SourceOrigin;
		std::string SourcePacking;
		std::string SourceProvenance;
		// not_inspected, finite, finite_overflow, unsupported_source_layout,
		// contains_infinity, contains_nan or contains_infinity_and_nan.
		std::string ValueClassification = "not_inspected";
		std::string Encoding;
		std::optional<double> MaximumAbsoluteError;
		std::string ColourSpace;
		std::string Origin;
		// Exact source storage for packed channels, such as rgba8_unorm.
		std::string Packing;
		// Empty when a channel has no extra estimator provenance.
		std::string Provenance;
		std::optional<DataCaptureBridgeAmbientOcclusion> AmbientOcclusion;
		std::optional<DataCaptureBridgeNoise> Noise;
		// Motion is measured against this renderer frame, not the preceding ticket.
		std::optional<uint64_t> PreviousCameraMotionFrame;
	};
	struct DataCaptureBridgeObjectLabel {
		uint32_t Label = 0;
		std::string StableId;
	};

	// Per-ticket facts collected at the boundaries that actually move capture
	// bytes. Optional values remain unavailable when the renderer has no
	// completed timestamp or allocator counter to report.
	struct DataCaptureBridgeProfile {
		uint64_t SourceBytes = 0;
		uint64_t RetainedBytes = 0;
		uint64_t ReadbackBytes = 0;
		uint64_t TransferBytes = 0;
		uint64_t SourceOperations = 0;
		uint64_t RetainedOperations = 0;
		uint64_t ReadbackOperations = 0;
		uint64_t TransferOperations = 0;
		std::optional<uint64_t> CpuReadbackNanoseconds;
		std::optional<uint64_t> CpuFinalizationNanoseconds;
		std::optional<double> FinalizationBytesPerSecond;
		std::optional<uint64_t> GpuNanoseconds;
		std::string GpuTimingReason = "unavailable/no_completed_gpu_timestamp";
		// Ticket-scoped capacity at the renderer's completed-readback boundary.
		// These fields remain separate because host vector storage and device staging
		// buffers are unlike pools. Neither is an allocation-event count, allocator
		// peak, process heap total, or driver memory total.
		std::optional<uint64_t> HostReadbackReservedCapacityBytes;
		std::optional<uint64_t> DeviceReadbackStagingReservedCapacityBytes;
		std::optional<uint64_t> AllocationBytes;
		std::optional<uint64_t> PeakAllocationBytes;
		std::string AllocationReason = "unavailable/no_capture_allocator_counter";
	};

	struct DataCaptureBridgePoll {
		std::string Status;
		std::string SnapshotId;
		uint64_t CaptureFrame = 0;
		// Describes the stored plane bytes even when no scene sidecar was requested.
		std::string StorageProfile = "lossless";

		// Camera values are copied with the completed capture so a consumer never
		// infers calibration from a later live view. Matrices are column-major.
		bool HasCamera = false;
		std::array<double, 16> WorldFromCamera{};
		bool HasProjection = false;
		std::array<double, 16> Projection{};
		double VerticalFieldOfViewRadians = 0.0;
		double NearMetres = 0.0;
		double FarMetres = 0.0;
		double CropLeft = 0.0;
		double CropTop = 0.0;
		double CropWidth = 1.0;
		double CropHeight = 1.0;
		std::string CropConvention = "normalized_full_view_left_top_width_height";
		// The current capture path has no lens or temporal jitter model. These
		// flags make that absence explicit instead of fabricating calibration.
		bool LensDistortionAvailable = false;
		std::string LensDistortionReason = "unavailable";
		bool JitterAvailable = false;
		std::string JitterPolicy = "unavailable";
		std::string CoordinateConvention;
		std::vector<DataCaptureBridgePlane> Planes;
		std::vector<DataCaptureBridgeObjectLabel> ObjectLabels;
		std::vector<DataCaptureBridgeObjectLabel> SemanticLabels;
		std::vector<DataCaptureBridgeObjectLabel> PartLabels;
		std::optional<DataCaptureBridgeSceneSidecar> SceneSidecar;
		DataCaptureBridgeProfile Profile;
	};

	struct DataCaptureBridgeHookCapability {
		std::string Name;
		uint32_t SchemaVersion = 0;
		std::string NodeKind;
		bool Required = false;
		std::vector<std::string> Channels;
		std::string Access = "observation";
		std::vector<std::string> MutatedFields;
	};
	struct ViewCameraMutationRequest {
		std::string InstanceId;
		std::string SnapshotId;
		std::string Pipeline;
		uint64_t PipelineRevision = 0;
		uint64_t ViewSlot = 0;
		std::optional<std::array<float, 7>> CameraFrame;
		struct Camera {
			float FieldOfViewRadians = 0.0f;
			float NearPlane = 0.0f;
			float FarPlane = 0.0f;
			uint32_t MaxImageWidth = 0;
			uint32_t MaxImageHeight = 0;
			uint32_t ImageWidth = 0;
			uint32_t ImageHeight = 0;
		};
		std::optional<Camera> Lens;
		std::optional<std::array<float, 16>> Projection;
	};
	struct ViewCameraMutationPoll {
		std::string Status;
		bool Terminal = false;
		std::string Detail;
	};
	struct DataCaptureBridgeCapabilities {
		bool Available = false;
		std::vector<std::string> Channels;
		std::vector<std::string> StorageProfiles;
		// Bounded machine-readable constraints for the training_compact profile.
		std::vector<std::string> TrainingCompactLimitations;
		std::vector<std::string> NoiseLimitations;
		// Stable render hook contracts. They are strings and fixed limits, never
		// renderer handles or process-local enum values.
		std::vector<DataCaptureBridgeHookCapability> HookRecords;
		uint32_t MaximumHooks = 0;
		uint32_t MaximumConnections = 0;
		uint32_t MaximumBatches = 0;
		// Queue admission is separately bounded from live hook connections and
		// readback batches. A coordinated multi-camera request cannot exceed it.
		uint32_t MaximumCaptureTickets = 0;
		uint32_t MaximumReadbackNodes = 0;
		uint64_t MaximumRetainedBytes = 0;
		uint32_t MaximumPendingPumps = 0;
		bool NamedCameraSelection = false;
		// A group records several camera views from one admitted render frame.
		// It says nothing about physical GPU overlap between those views.
		bool SameFrameMultiCamera = false;
		uint32_t MaximumSameFrameCameraViews = 0;
		uint32_t MaximumCameraIdBytes = 0;
		std::string Detail;
	};

	class DataCaptureBridge {
	  public:
		virtual ~DataCaptureBridge() = default;
		virtual DataCaptureBridgeCapabilities Capabilities() const = 0;
		virtual bool Queue(
			std::string_view instanceId,
			const DataCaptureBridgeRequest &request,
			uint64_t &ticket,
			std::string &detail
		) = 0;
		// Admits a coordinated camera group atomically. Every request must name
		// one instance, snapshot, pipeline, capture node, and logical view slot.
		// Each camera id must be unique. Implementations that do not provide one
		// render-frame group refuse without writing tickets.
		virtual bool QueueGroup(
			std::string_view,
			std::span<const DataCaptureBridgeRequest>,
			std::span<uint64_t> tickets,
			std::string &detail
		) {
			std::fill(tickets.begin(), tickets.end(), uint64_t{0});
			detail = "same-frame multi-camera capture is unavailable";
			return false;
		}
		virtual bool Poll(
			std::string_view instanceId, uint64_t ticket, DataCaptureBridgePoll &poll, std::string &detail
		) = 0;

		// Copies one bounded byte range from a retained completed plane. `resource`
		// is the opaque ticket-scoped identifier returned by Poll.
		virtual bool ReadPlane(
			std::string_view instanceId,
			uint64_t ticket,
			std::string_view resource,
			size_t offset,
			size_t maximumBytes,
			std::vector<std::byte> &bytes,
			std::string &detail
		) = 0;

		// Releases a terminal capture and its retained plane bytes. Pending
		// captures must be cancelled and observed terminal before release.
		virtual bool Release(std::string_view instanceId, uint64_t ticket, std::string &detail) = 0;
		virtual void Cancel(std::string_view instanceId, uint64_t ticket) = 0;
		// A one-shot typed mutation ticket. Hosts that do not provide the renderer
		// hook return false rather than pretending a queued patch will run.
		virtual bool QueueViewCameraMutation(
			std::string_view, const ViewCameraMutationRequest &, uint64_t &, std::string &detail
		) {
			detail = "view.camera is unavailable";
			return false;
		}
		virtual void CancelViewCameraMutation(std::string_view, uint64_t) {}
		virtual bool
		PollViewCameraMutation(std::string_view, uint64_t, ViewCameraMutationPoll &, std::string &detail) {
			detail = "view.camera is unavailable";
			return false;
		}
	};

}
