#pragma once

// Immutable renderer capture records for data-producing clients. This is a
// renderer boundary, not a statement that every named data channel exists.

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/ResourceImage.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::render {
	// Stable identity assigned to one captured object label.
	struct DataCaptureObjectLabel {
		// One-based label value written into the capture plane.
		uint32_t Label = 0;
		// Stable object identity associated with Label.
		std::string StableId;
	};
	// Semantic class labels use the same stable-id representation as objects.
	using DataCaptureSemanticLabel = DataCaptureObjectLabel;
	// Part labels use the same stable-id representation as objects.
	using DataCapturePartLabel = DataCaptureObjectLabel;
	// Maximum labels accepted in one capture descriptor.
	inline constexpr size_t MAX_DATA_CAPTURE_OBJECT_LABELS = 4096;
	// Maximum combined UTF-8 bytes in a label table.
	inline constexpr size_t MAX_DATA_CAPTURE_OBJECT_LABEL_BYTES = 512 * 1024;

	// Checks for well-formed UTF-8 without normalizing the input.
	inline bool DataCaptureUtf8(std::string_view value) {
		for (size_t offset = 0; offset < value.size();) {
			const uint8_t first = static_cast<uint8_t>(value[offset]);
			if (first < 0x80) {
				++offset;
				continue;
			}
			const size_t count = first >= 0xC2 && first <= 0xDF	  ? 2
								 : first >= 0xE0 && first <= 0xEF ? 3
								 : first >= 0xF0 && first <= 0xF4 ? 4
																  : 0;
			if (count == 0 || offset + count > value.size()) return false;
			for (size_t index = 1; index < count; ++index)
				if ((static_cast<uint8_t>(value[offset + index]) & 0xC0) != 0x80) return false;
			const uint32_t codepoint =
				count == 2	 ? (first & 0x1F) << 6 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F)
				: count == 3 ? (first & 0x0F) << 12 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F) << 6 |
								   (static_cast<uint8_t>(value[offset + 2]) & 0x3F)
							 : (first & 0x07) << 18 | (static_cast<uint8_t>(value[offset + 1]) & 0x3F) << 12 |
								   (static_cast<uint8_t>(value[offset + 2]) & 0x3F) << 6 |
								   (static_cast<uint8_t>(value[offset + 3]) & 0x3F);
			if ((count == 3 && codepoint < 0x800) || (count == 4 && codepoint < 0x10000) ||
				codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				return false;
			offset += count;
		}
		return true;
	}

	// Checks one-based labels, stable-id order, UTF-8 and size limits.
	inline bool ValidDataCaptureObjectLabels(std::span<const DataCaptureObjectLabel> labels) {
		if (labels.size() > MAX_DATA_CAPTURE_OBJECT_LABELS) return false;
		size_t bytes = 0;
		for (size_t index = 0; index < labels.size(); ++index) {
			const DataCaptureObjectLabel &label = labels[index];
			if (label.Label != index + 1 || label.StableId.empty() || label.StableId.size() > 256 ||
				label.StableId.find('\0') != std::string::npos || !DataCaptureUtf8(label.StableId) ||
				label.StableId.size() > MAX_DATA_CAPTURE_OBJECT_LABEL_BYTES - bytes)
				return false;
			bytes += label.StableId.size();
			if (index != 0 && !std::lexicographical_compare(
								  labels[index - 1].StableId.begin(),
								  labels[index - 1].StableId.end(),
								  label.StableId.begin(),
								  label.StableId.end(),
								  [](char left, char right) {
									  return static_cast<unsigned char>(left) <
											 static_cast<unsigned char>(right);
								  }
							  ))
				return false;
		}
		return true;
	}

	// Stable channel names are represented by this enum at the device boundary.
	// A channel only becomes Ready when its declared render-graph input was copied.
	enum class DataCaptureChannel : uint8_t {
		RgbLinearHdr,
		LinearDepth,
		ShadingNormal,
		PbrAlbedo,
		PbrMaterial,
		PbrEmissive,
		// Scalar authored factors; transmission is captured without physical refraction.
		PbrSpecular,
		PbrTransmission,
		// Perspective-correct interpolated authored mesh texcoords at visible opaque pixels.
		MeshUv,
		// Screen-space ambient-occlusion estimator visibility, not ground truth.
		AmbientOcclusion,
		ObjectIds,
		SemanticMask,
		PartMask,
		FirstSurfaceValidity,
		SecondSurfaceDepth,
		SecondSurfaceValidity,
		MotionVectors,
		OpticalFlow,
		// Unshadowed directional radiance in RGB and the exact sampled visibility factor in alpha.
		DirectionalResponse,
		// One-byte display/export representation derived from DirectionalResponse alpha.
		ShadowVisibility,
		// Explicit RGBA32F render-graph packing, with four documented retained lanes.
		PackedGpu,
	};

	// Lifecycle and terminal outcomes of a capture channel.
	enum class DataCaptureStatus : uint8_t {
		Pending,
		Ready,
		Partial,
		Unsupported,
		Invalid,
		Failed,
		Cancelled,
	};
	// Storage formats for values in a captured plane.
	enum class DataCaptureScalar : uint8_t { Float16, Float32, UInt32, UNorm8, UNorm10A2, Unknown };
	// Colour transfer interpretations for a captured plane.
	enum class DataCaptureColourSpace : uint8_t { Linear, SRGB, NotApplicable, Unknown };
	// Pixel-coordinate origin used when reading a captured plane.
	enum class DataCaptureOrigin : uint8_t { TopLeft };
	// How a request handles renderer-local temporal history.
	enum class DataCaptureTemporalHistory : uint8_t { Preserve, Reset, Disable };
	// Derives the graph resource name for a capture channel.
	inline core::Name DataCaptureNode(core::Name base, DataCaptureChannel channel) {
		const std::string_view suffix =
			channel == DataCaptureChannel::PbrAlbedo			  ? "-albedo"
			: channel == DataCaptureChannel::PbrMaterial		  ? "-material"
			: channel == DataCaptureChannel::PbrEmissive		  ? "-emissive"
			: channel == DataCaptureChannel::PbrSpecular		  ? "-specular"
			: channel == DataCaptureChannel::PbrTransmission	  ? "-transmission"
			: channel == DataCaptureChannel::MeshUv				  ? "-mesh-uv"
			: channel == DataCaptureChannel::AmbientOcclusion	  ? "-ambient-occlusion"
			: channel == DataCaptureChannel::ObjectIds			  ? "-object-ids"
			: channel == DataCaptureChannel::SemanticMask		  ? "-semantic-ids"
			: channel == DataCaptureChannel::PartMask			  ? "-part-ids"
			: channel == DataCaptureChannel::FirstSurfaceValidity ? "-first-surface-validity"
			: channel == DataCaptureChannel::SecondSurfaceDepth ||
					channel == DataCaptureChannel::SecondSurfaceValidity
				? "-second-surface"
			: channel == DataCaptureChannel::MotionVectors ? "-motion-vectors"
			: channel == DataCaptureChannel::OpticalFlow   ? "-optical-flow"
			: channel == DataCaptureChannel::DirectionalResponse ||
					channel == DataCaptureChannel::ShadowVisibility
				? "-directional-response"
			: channel == DataCaptureChannel::PackedGpu ? "-packed-gpu"
													   : "";
		return suffix.empty() ? base : core::Name(std::string(base.Text()) + std::string(suffix));
	}

	// These conventions are fixed by scene::ResolveCamera and are repeated on
	// every result so an exported image cannot be separated from its coordinates.
	struct DataCaptureCameraConvention {
		// World coordinates use right-handed axes.
		bool RightHandedWorld = true;
		// Forward from the camera is local negative Z.
		bool CameraLooksNegativeZ = true;
		// Clip-space positive Y points up.
		bool ClipYUp = true;
		// Clip-space depth spans zero to one.
		bool DepthZeroToOne = true;
		// Projection matrices store their columns contiguously.
		bool ProjectionIsColumnMajor = true;
		// Scale from world units to metres.
		float MetresPerWorldUnit = 1.0f;
	};

	// Camera pose and projection recorded with a data capture.
	struct DataCaptureCamera {
		// Column-major world-from-camera transform captured with the draw.
		std::array<float, 16> WorldFromCamera{};
		// False when no camera projection could be captured.
		bool ProjectionAvailable = false;
		// Column-major camera projection matrix, when available.
		std::array<float, 16> Projection{};
		// Vertical field of view in radians.
		float VerticalFieldOfViewRadians = 0.0f;
		// Near clipping distance in metres.
		float NearPlaneMetres = 0.0f;
		// Far clipping distance in metres.
		float FarPlaneMetres = 0.0f;
		// Left edge of the normalized projection crop.
		float CropLeft = 0.0f;
		// Top edge of the normalized projection crop.
		float CropTop = 0.0f;
		// Width of the normalized projection crop.
		float CropWidth = 1.0f;
		// Height of the normalized projection crop.
		float CropHeight = 1.0f;
	};

	// Channels and view identity requested for a data capture.
	struct DataCaptureRequest {
		// A stable snapshot identity supplied by the world owner. It is copied
		// into every descriptor and never inferred from a renderer frame number.
		std::string SnapshotId;
		// Pipeline whose graph supplies the requested channels.
		core::Name Pipeline;
		// Capture node selected within Pipeline.
		core::Name CaptureNode;
		// Renderer view slot to capture.
		size_t ViewSlot = 0;
		// Channels requested from the selected graph node.
		std::vector<DataCaptureChannel> Channels;
		// Object identities to include beside object-id pixels.
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		// Semantic identities to include beside semantic-mask pixels.
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		// Part identities to include beside part-mask pixels.
		std::vector<DataCapturePartLabel> PartLabels;
		// History policy for temporal capture channels.
		DataCaptureTemporalHistory TemporalHistory = DataCaptureTemporalHistory::Preserve;
	};

	// One image plane and its format and provenance.
	struct DataCapturePlane {
		// Channel that produced this plane.
		DataCaptureChannel Channel = DataCaptureChannel::RgbLinearHdr;
		// Whether the channel is ready, partial or terminal.
		DataCaptureStatus Status = DataCaptureStatus::Pending;
		// Graph resource copied into this plane.
		core::Name Resource;
		// Graph node that produced Resource.
		core::Name CaptureNode;
		// Number of columns in the captured image.
		uint32_t Width = 0;
		// Number of rows in the captured image.
		uint32_t Height = 0;
		// Byte stride between consecutive image rows.
		uint32_t RowStride = 0;
		// Storage type of each encoded pixel component.
		DataCaptureScalar Scalar = DataCaptureScalar::Unknown;
		// Colour-space interpretation of encoded pixels.
		DataCaptureColourSpace ColourSpace = DataCaptureColourSpace::Unknown;
		// Corner used as pixel coordinate zero.
		DataCaptureOrigin Origin = DataCaptureOrigin::TopLeft;
		// Present only for the ambient-occlusion plane, including an explicit
		// Unavailable state for an unrecognised R8 source.
		std::optional<AmbientOcclusionProvenance> AmbientOcclusion;
		// Why this channel has its reported status or contents.
		std::string Provenance;
		// Renderer-local preceding frame used by a verified camera-motion plane.
		// It is absent for non-temporal planes and cannot be compared across renderers.
		std::optional<uint64_t> PreviousCameraMotionFrame;
		// BLAKE3-256 of Bytes. It is zero until this plane is Ready.
		assets::ContentHash Hash;
		// Owned pixel bytes, present once the plane is ready.
		std::vector<std::byte> Bytes;
	};

	// Caller-owned ticket. The renderer only receives its resource-image tokens,
	// so cancellation and lifetime stay explicit at the product boundary.
	struct DataCaptureTicket {
		// World snapshot to which all ticket results belong.
		std::string SnapshotId;
		// Pipeline selected when the ticket was queued.
		core::Name Pipeline;
		// Capture node selected when the ticket was queued.
		core::Name CaptureNode;
		// View slot selected when the ticket was queued.
		size_t ViewSlot = 0;
		// History policy retained until the ticket completes.
		DataCaptureTemporalHistory TemporalHistory = DataCaptureTemporalHistory::Preserve;
		// Logical channels awaiting resource readback.
		std::vector<DataCaptureChannel> Channels;
		// Object label table retained for the final result.
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		// Semantic label table retained for the final result.
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		// Part label table retained for the final result.
		std::vector<DataCapturePartLabel> PartLabels;
		// One entry per logical channel, indexing ResourceTokens. Channels emitted
		// by one capture node share a single GPU readback.
		std::vector<uint8_t> ChannelResourceIndices;
		// Renderer readback tokens shared by logical channels.
		std::vector<uint64_t> ResourceTokens;
		// Timestamp query identifier for capture GPU work.
		uint64_t GpuTimingId = 0;
		// Readbacks already completed for this ticket.
		std::vector<ResourceImage> CompletedImages;
		// Whether the caller has taken the completed images.
		bool ImagesTaken = false;
		// Whether pending readbacks should be discarded.
		bool Cancelled = false;
	};

	// The current result of a data capture ticket.
	struct DataCapturePoll {
		// Overall outcome of polling the capture ticket.
		DataCaptureStatus Status = DataCaptureStatus::Invalid;
		// World snapshot reported by this result.
		std::string SnapshotId;
		// Renderer frame that produced the capture.
		uint64_t CaptureFrame = 0;
		// Pipeline used to produce the capture.
		core::Name Pipeline;
		// Revision of the pipeline used for the capture.
		uint64_t PipelineRevision = 0;
		// World that supplied the captured view.
		core::Name WorldName;
		// View slot used to produce the capture.
		size_t ViewSlot = 0;
		// This is false only for Preserve. Reset and Disable are refused until a
		// render-only pass applies their declared renderer-local history policy.
		bool TemporalHistoryChanged = false;
		// Coordinate conventions attached to every result.
		DataCaptureCameraConvention Camera;
		// Camera transform, projection and crop used for this result.
		DataCaptureCamera CameraPose;
		// Object-id lookup table for the returned planes.
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		// Semantic-mask lookup table for the returned planes.
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		// Part-mask lookup table for the returned planes.
		std::vector<DataCapturePartLabel> PartLabels;
		// One result descriptor per requested capture channel.
		std::vector<DataCapturePlane> Planes;
		// CPU time spent taking completed readback images into this poll.
		std::optional<uint64_t> CpuReadbackNanoseconds;
		// Sum of the completed ticket's ResourceImage owned-vector capacities. It
		// excludes later storage transforms and is neither a process heap total nor
		// an allocator high-water mark.
		uint64_t HostReadbackReservedCapacityBytes = 0;
		// Sum of the tracked GPU transfer-buffer capacities reserved by the ticket's
		// unique copied images while their downloads were recorded. It can include a
		// buffer reused from an earlier ticket and is not a driver memory total.
		uint64_t DeviceReadbackStagingReservedCapacityBytes = 0;
		// Completed GPU duration in nanoseconds, when available.
		std::optional<uint64_t> GpuNanoseconds;
		// Reason a GPU duration is unavailable.
		std::string GpuTimingReason = "unavailable/no_completed_gpu_timestamp";
	};

	// Returns the stable text name of a capture channel.
	const char *DataCaptureChannelName(DataCaptureChannel channel);
	// Returns the fixed renderer camera-coordinate conventions.
	DataCaptureCameraConvention DataCaptureCameraConventions();
}
