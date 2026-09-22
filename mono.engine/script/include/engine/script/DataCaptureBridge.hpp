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
	// Maximum derived RGBA32F outputs retained by one capture ticket.
	inline constexpr size_t MAX_DATA_CAPTURE_PACKED_PLANES = 3;
	// Selects one component from a named completed capture channel.
	struct DataCaptureBridgePackedComponent {
		// Channel whose retained bytes supply this packed component.
		std::string SourceChannel;
		// Zero-based component index in the retained source plane's pixel packing.
		uint8_t SourceComponent = 0;
	};

	// Describes one opt-in RGBA32F plane assembled from completed capture channels.
	struct DataCaptureBridgePackedPlane {
		// Stable ticket-local output name. It becomes the returned plane channel name.
		std::string Name;
		// Source selection for output R, G, B, and A in that order.
		std::array<DataCaptureBridgePackedComponent, 4> Components;
	};

	// A copied request exchanged with the capture host.
	struct DataCaptureBridgeRequest {
		// Data-factory instance identifier.
		std::string InstanceId;
		// Snapshot that produced this record.
		std::string SnapshotId;
		// DataFactory pipeline name that owns the capture node.
		std::string Pipeline;
		// Stable node name within `Pipeline` that supplies the capture.
		std::string CaptureNode;
		// `current_view` keeps the host's selected view. Any other value is a
		// stable authored DataFactoryId for one Camera and Transform in the world.
		std::string CameraId = "current_view";
		// Logical view index used to correlate this request with grouped views.
		uint64_t ViewSlot = 0;
		// Channel names the host must retain for this ticket.
		std::vector<std::string> Channels;
		// Stable DataFactoryIds whose local-light terms are requested with local_light_contribution.
		std::vector<std::string> LocalLightIds;
		// Requested temporal-history policy, as named by the capture host.
		std::string TemporalHistory;
		// "lossless" retains renderer readback bytes. "training_compact" is an
		// explicit storage transform with per-plane encoding metadata.
		std::string StorageProfile = "lossless";
		// `gaussian` applies a deterministic bounded perturbation to copied
		// rgb_linear_hdr colour lanes only. It never changes alpha or a label plane.
		std::string NoiseMode = "none";
		// Deterministic seed used when `NoiseMode` requests perturbation.
		uint64_t NoiseSeed = 0;
		// Requested noise standard deviation in the channel's normalized units.
		double NoiseSigma = 0.0;
		// The renderer copies a scene sidecar only after its retained-snapshot
		// barrier succeeds. This flag asks for that bounded copied observation.
		bool IncludeSceneData = false;
		// Optional derived outputs assembled after their source planes complete. The
		// original channel planes remain available alongside every packed plane.
		std::vector<DataCaptureBridgePackedPlane> PackedPlanes;
	};

	// A scene observation retained with one capture ticket. The lifecycle values
	// identify the exact paused state from which `Scene` was copied.
	struct DataCaptureBridgeSceneSidecar {
		// Snapshot that produced this record.
		std::string SnapshotId;
		// World tick for this record.
		uint64_t Tick = 0;
		// World lifetime epoch associated with the copied scene state.
		uint64_t WorldEpoch = 0;
		// Monotonic world state version associated with the copied scene.
		uint64_t WorldVersion = 0;
		// Encoding profile used for the retained scene payload.
		std::string StorageProfile = "lossless";
		// Copied scene value owned by this completed ticket.
		ScriptValue Scene;
	};

	// A copied ambient occlusion exchanged with the capture host.
	struct DataCaptureBridgeAmbientOcclusion {
		// Renderer state used to produce the ambient occlusion values.
		std::string SourceState;
		// Renderer frame that produced the values, when a completed frame exists.
		std::optional<uint64_t> ProducerFrame;
		// Whether this feature is enabled.
		std::optional<bool> Enabled;
		// Number of occlusion samples used by the producer, when reported.
		std::optional<uint32_t> SampleCount;
		// Occlusion search radius in world units, when reported.
		std::optional<double> RadiusWorldUnits;
		// Name of the denoiser applied by the producer, when any.
		std::optional<std::string> Denoiser;
		// Temporal accumulation policy used by the producer, when reported.
		std::optional<std::string> TemporalHistory;
		// Occlusion value used for background pixels, when reported.
		std::optional<double> BackgroundValue;
		// Machine-readable classification of the background value.
		std::optional<std::string> BackgroundClassification;
	};

	// Describes the copied RGB perturbation. The native renderer bytes remain
	// addressable through the source descriptor on the same plane.
	struct DataCaptureBridgeNoise {
		// Requested or applied noise mode, such as `none` or `gaussian`.
		std::string Mode;
		// Concrete deterministic noise algorithm used for the copied plane.
		std::string Algorithm;
		// Deterministic package seed.
		uint64_t Seed = 0;
		// Requested standard deviation before quantization.
		double Sigma = 0.0;
		// Rule used to quantize the requested standard deviation.
		std::string SigmaQuantization;
		// Quantized standard deviation stored as a fixed point Q24 value.
		uint64_t EffectiveSigmaQ24 = 0;
		// Actual standard deviation after quantization in normalized units.
		double EffectiveSigma = 0.0;
		// Rule describing how the seed advances across pixels and frames.
		std::string SeedStatePolicy;
		// Order in which perturbation is applied relative to value conversion.
		std::string Order;
		// Rule used to clamp perturbed channel values.
		std::string ClampPolicy;
		// Rule describing whether alpha is preserved or changed.
		std::string AlphaPolicy;
		// Classification of the copied values after perturbation.
		std::string ValueClassification;
		// Maximum absolute error introduced by the stored representation.
		std::optional<double> MaximumAbsoluteError;
	};

	// A copied plane exchanged with the capture host.
	struct DataCaptureBridgePlane {
		// Stable channel name represented by this plane.
		std::string Channel;
		// Stable source light for local_light_contribution; empty for other channels.
		std::string LightId = {};
		// Machine-readable operation status.
		std::string Status;
		// An opaque capture-local identifier. It is accepted only with its
		// owning ticket and does not name a renderer graph resource.
		std::string Resource;
		// Source-side resource identifier for the renderer readback.
		std::string SourceResource;
		// Algorithm used to compute `Hash` and `SourceHash`.
		std::string HashAlgorithm;
		// Content hash of the associated payload.
		std::string Hash;
		// Image width in pixels.
		uint32_t Width = 0;
		// Image height in pixels.
		uint32_t Height = 0;
		// Bytes between successive image rows in the retained payload.
		uint32_t RowStride = 0;
		// Number of retained payload bytes available through `ReadPlane`.
		size_t ByteSize = 0;
		// Scalar representation of the retained channel values.
		std::string Scalar;
		// SourceScalar is the source value representation. Scalar and Encoding
		// describe the retained bytes returned by ReadPlane.
		std::string SourceScalar;
		// Content hash of the source renderer payload.
		std::string SourceHash;
		// Source image width in pixels before retention conversion.
		uint32_t SourceWidth = 0;
		// Source image height in pixels before retention conversion.
		uint32_t SourceHeight = 0;
		// Source row stride in bytes before retention conversion.
		uint32_t SourceRowStride = 0;
		// Source payload size in bytes before retention conversion.
		size_t SourceByteSize = 0;
		// Source pixel encoding before the retained representation.
		std::string SourceEncoding;
		// Colour space of the source renderer payload.
		std::string SourceColourSpace;
		// Coordinate origin used by the source renderer payload.
		std::string SourceOrigin;
		// Channel packing of the source renderer payload.
		std::string SourcePacking;
		// Source-side provenance for the captured channel.
		std::string SourceProvenance;
		// not_inspected, finite, finite_overflow, unsupported_source_layout,
		// contains_infinity, contains_nan or contains_infinity_and_nan.
		std::string ValueClassification = "not_inspected";
		// Pixel encoding of the retained payload.
		std::string Encoding;
		// Maximum absolute error introduced by retention conversion.
		std::optional<double> MaximumAbsoluteError;
		// Colour space of the retained payload.
		std::string ColourSpace;
		// World-space origin used when the renderer queried this plane.
		std::string Origin;
		// Exact source storage for packed channels, such as rgba8_unorm.
		std::string Packing;
		// Empty when a channel has no extra estimator provenance.
		std::string Provenance;
		// Optional ambient occlusion metadata for this channel.
		std::optional<DataCaptureBridgeAmbientOcclusion> AmbientOcclusion;
		// Optional perturbation metadata for this channel.
		std::optional<DataCaptureBridgeNoise> Noise;
		// Motion is measured against this renderer frame, not the preceding ticket.
		std::optional<uint64_t> PreviousCameraMotionFrame;
		// Present for a derived RGBA32F plane and records the source selected for
		// each output component.
		std::optional<DataCaptureBridgePackedPlane> Packed;
		// Pixel-center nearest-neighbour resampling used by a packed plane, or empty
		// when this is a native capture plane.
		std::string Resampling;
	};
	// A copied object label exchanged with the capture host.
	struct DataCaptureBridgeObjectLabel {
		// Numeric label value assigned by the capture producer.
		uint32_t Label = 0;
		// Stable authored object identity used to join labels across captures.
		std::string StableId;
	};

	// Per-ticket facts collected at the boundaries that actually move capture
	// bytes. Optional values remain unavailable when the renderer has no
	// completed timestamp or allocator counter to report.
	struct DataCaptureBridgeProfile {
		// Bytes read from the renderer's source resources.
		uint64_t SourceBytes = 0;
		// Bytes copied into ticket-owned retained storage.
		uint64_t RetainedBytes = 0;
		// Bytes copied from device readback into host memory.
		uint64_t ReadbackBytes = 0;
		// Bytes transferred between capture staging boundaries.
		uint64_t TransferBytes = 0;
		// Number of source-resource operations performed for this ticket.
		uint64_t SourceOperations = 0;
		// Number of retained-storage operations performed for this ticket.
		uint64_t RetainedOperations = 0;
		// Number of device-to-host readback operations for this ticket.
		uint64_t ReadbackOperations = 0;
		// Number of staging transfer operations for this ticket.
		uint64_t TransferOperations = 0;
		// CPU time spent copying device readback, in nanoseconds.
		std::optional<uint64_t> CpuReadbackNanoseconds;
		// CPU time spent finalizing the ticket, in nanoseconds.
		std::optional<uint64_t> CpuFinalizationNanoseconds;
		// Finalization throughput computed from retained bytes per second.
		std::optional<double> FinalizationBytesPerSecond;
		// Completed GPU duration for this ticket, in nanoseconds.
		std::optional<uint64_t> GpuNanoseconds;
		// Reason GPU duration is unavailable when no completed timestamp exists.
		std::string GpuTimingReason = "unavailable/no_completed_gpu_timestamp";
		// Ticket-scoped capacity at the renderer's completed-readback boundary.
		// These fields remain separate because host vector storage and device staging
		// buffers are unlike pools. Neither is an allocation-event count, allocator
		// peak, process heap total, or driver memory total.
		std::optional<uint64_t> HostReadbackReservedCapacityBytes;
		// Device readback staging reserved capacity bytes.
		std::optional<uint64_t> DeviceReadbackStagingReservedCapacityBytes;
		// Total bytes reported by the capture allocator, when available.
		std::optional<uint64_t> AllocationBytes;
		// Peak bytes reported by the capture allocator, when available.
		std::optional<uint64_t> PeakAllocationBytes;
		// Reason allocation counters are unavailable for this host.
		std::string AllocationReason = "unavailable/no_capture_allocator_counter";
	};

	// A copied poll exchanged with the capture host.
	struct DataCaptureBridgePoll {
		// Machine-readable operation status.
		std::string Status;
		// Snapshot that produced this record.
		std::string SnapshotId;
		// Renderer frame that completed this capture.
		uint64_t CaptureFrame = 0;
		// Describes the stored plane bytes even when no scene sidecar was requested.
		std::string StorageProfile = "lossless";

		// Camera values are copied with the completed capture so a consumer never
		// infers calibration from a later live view. Matrices are column-major.
		bool HasCamera = false;
		// Column-major transform from camera space to world space.
		std::array<double, 16> WorldFromCamera{};
		// True when `Projection` contains calibration for this capture.
		bool HasProjection = false;
		// Column-major projection matrix copied with the completed capture.
		std::array<double, 16> Projection{};
		// Vertical field of view radians.
		double VerticalFieldOfViewRadians = 0.0;
		// Near clipping distance in metres.
		double NearMetres = 0.0;
		// Far clipping distance in metres.
		double FarMetres = 0.0;
		// Normalized crop left edge in the full view.
		double CropLeft = 0.0;
		// Normalized crop top edge in the full view.
		double CropTop = 0.0;
		// Normalized crop width relative to the full view.
		double CropWidth = 1.0;
		// Normalized crop height relative to the full view.
		double CropHeight = 1.0;
		// Meaning and order of the normalized crop values.
		std::string CropConvention = "normalized_full_view_left_top_width_height";
		// The current capture path has no lens or temporal jitter model. These
		// flags make that absence explicit instead of fabricating calibration.
		bool LensDistortionAvailable = false;
		// Reason lens distortion data is unavailable.
		std::string LensDistortionReason = "unavailable";
		// True when temporal projection jitter was captured.
		bool JitterAvailable = false;
		// Policy used for temporal projection jitter, or `unavailable`.
		std::string JitterPolicy = "unavailable";
		// Coordinate convention used by matrices and plane origins.
		std::string CoordinateConvention;
		// Copied image planes returned by the completed ticket.
		std::vector<DataCaptureBridgePlane> Planes;
		// Object labels aligned with the captured label plane.
		std::vector<DataCaptureBridgeObjectLabel> ObjectLabels;
		// Semantic labels aligned with the captured label plane.
		std::vector<DataCaptureBridgeObjectLabel> SemanticLabels;
		// Part labels aligned with the captured label plane.
		std::vector<DataCaptureBridgeObjectLabel> PartLabels;
		// Optional scene copy captured at the same retained snapshot.
		std::optional<DataCaptureBridgeSceneSidecar> SceneSidecar;
		// Byte, operation, and timing counters for this ticket.
		DataCaptureBridgeProfile Profile;
	};

	// A copied hook capability exchanged with the capture host.
	struct DataCaptureBridgeHookCapability {
		// Stable name scripts use when referring to this hook.
		std::string Name;
		// Version of the hook contract described by this record.
		uint32_t SchemaVersion = 0;
		// Stable kind name of the render graph node exposing this hook.
		std::string NodeKind;
		// Whether the host must provide this hook for the capability to be valid.
		bool Required = false;
		// Requested capture channels.
		std::vector<std::string> Channels;
		// Access mode granted to scripts for this hook.
		std::string Access = "observation";
		// Field names the hook is permitted to mutate, if any.
		std::vector<std::string> MutatedFields;
	};
	// A copied camera-mutation request exchanged with the capture host.
	struct ViewCameraMutationRequest {
		// Data-factory instance identifier.
		std::string InstanceId;
		// Snapshot that produced this record.
		std::string SnapshotId;
		// DataFactory pipeline whose camera is being mutated.
		std::string Pipeline;
		// Pipeline revision.
		uint64_t PipelineRevision = 0;
		// Logical view index associated with this mutation.
		uint64_t ViewSlot = 0;
		// Optional packed camera frame values supplied by the caller.
		std::optional<std::array<float, 7>> CameraFrame;
		// Camera values supplied as one coherent mutation.
		struct Camera {
			// Vertical camera field of view in radians.
			float FieldOfViewRadians = 0.0f;
			// Near clipping distance in world units.
			float NearPlane = 0.0f;
			// Far clipping distance in world units.
			float FarPlane = 0.0f;
			// Maximum image width accepted by the mutation, in pixels.
			uint32_t MaxImageWidth = 0;
			// Maximum image height accepted by the mutation, in pixels.
			uint32_t MaxImageHeight = 0;
			// Requested image width in pixels.
			uint32_t ImageWidth = 0;
			// Requested image height in pixels.
			uint32_t ImageHeight = 0;
		};
		// Optional camera lens settings.
		std::optional<Camera> Lens;
		// Optional column-major projection matrix for the mutated camera.
		std::optional<std::array<float, 16>> Projection;
	};
	// A copied camera-mutation poll exchanged with the capture host.
	struct ViewCameraMutationPoll {
		// Machine-readable operation status.
		std::string Status;
		// True when the mutation has reached a terminal state.
		bool Terminal = false;
		// Human-readable diagnostic.
		std::string Detail;
	};
	// A copied capabilities exchanged with the capture host.
	struct DataCaptureBridgeCapabilities {
		// Whether this capability is available.
		bool Available = false;
		// Channel names supported by the capture host.
		std::vector<std::string> Channels;
		// Retention profiles accepted by capture requests.
		std::vector<std::string> StorageProfiles;
		// Bounded machine-readable constraints for the training_compact profile.
		std::vector<std::string> TrainingCompactLimitations;
		// Machine-readable limits on supported noise modes.
		std::vector<std::string> NoiseLimitations;
		// Stable render hook contracts. They are strings and fixed limits, never
		// renderer handles or process-local enum values.
		std::vector<DataCaptureBridgeHookCapability> HookRecords;
		// Maximum concurrently registered render hooks.
		uint32_t MaximumHooks = 0;
		// Maximum live signal connections across hooks.
		uint32_t MaximumConnections = 0;
		// Maximum capture batches admitted at once.
		uint32_t MaximumBatches = 0;
		// Queue admission is separately bounded from live hook connections and
		// readback batches. A coordinated multi-camera request cannot exceed it.
		uint32_t MaximumCaptureTickets = 0;
		// Maximum readback nodes retained by one host.
		uint32_t MaximumReadbackNodes = 0;
		// Maximum bytes retained across pending and completed tickets.
		uint64_t MaximumRetainedBytes = 0;
		// Maximum delivery pumps waiting for host processing.
		uint32_t MaximumPendingPumps = 0;
		// Whether requests may select authored camera ids.
		bool NamedCameraSelection = false;
		// A group records several camera views from one admitted render frame.
		// It says nothing about physical GPU overlap between those views.
		bool SameFrameMultiCamera = false;
		// Maximum same frame camera views.
		uint32_t MaximumSameFrameCameraViews = 0;
		// Maximum UTF-8 bytes accepted in one camera id.
		uint32_t MaximumCameraIdBytes = 0;
		uint32_t MaximumLocalLightIds = 0;
		// Host diagnostic for capability negotiation or failure.
		std::string Detail;
	};

	// A copied  exchanged with the capture host.
	class DataCaptureBridge {
	  public:
		virtual ~DataCaptureBridge() = default;
		// Returns the host's supported channels, limits, and hook contracts.
		virtual DataCaptureBridgeCapabilities Capabilities() const = 0;
		// Queues a lifecycle operation for the host.
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
		// Reads the current lifecycle operation result.
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
		// Cancels a pending ticket; completed tickets remain terminal until release.
		virtual void Cancel(std::string_view instanceId, uint64_t ticket) = 0;
		// A one-shot typed mutation ticket. Hosts that do not provide the renderer
		// hook return false rather than pretending a queued patch will run.
		virtual bool QueueViewCameraMutation(
			std::string_view, const ViewCameraMutationRequest &, uint64_t &, std::string &detail
		) {
			detail = "view.camera is unavailable";
			return false;
		}
		// Cancels a queued camera mutation.
		virtual void CancelViewCameraMutation(std::string_view, uint64_t) {}
		// Polls a queued camera mutation.
		virtual bool
		PollViewCameraMutation(std::string_view, uint64_t, ViewCameraMutationPoll &, std::string &detail) {
			detail = "view.camera is unavailable";
			return false;
		}
	};

}
