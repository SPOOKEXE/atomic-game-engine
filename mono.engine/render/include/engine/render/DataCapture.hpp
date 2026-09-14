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
	struct DataCaptureObjectLabel {
		uint32_t Label = 0;
		std::string StableId;
	};
	using DataCaptureSemanticLabel = DataCaptureObjectLabel;
	using DataCapturePartLabel = DataCaptureObjectLabel;
	inline constexpr size_t MAX_DATA_CAPTURE_OBJECT_LABELS = 4096;
	inline constexpr size_t MAX_DATA_CAPTURE_OBJECT_LABEL_BYTES = 512 * 1024;

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
		// The current material model has no authored specular or transmission fact.
		// They remain requestable so consumers receive a terminal provenance record.
		PbrSpecular,
		PbrTransmission,
		// Perspective-correct interpolated authored mesh texcoords at visible opaque pixels.
		MeshUv,
		// Screen-space ambient-occlusion estimator visibility, not ground truth.
		AmbientOcclusion,
		ObjectIds,
		SemanticMask,
		PartMask,
		SecondSurfaceDepth,
		SecondSurfaceValidity,
		MotionVectors,
		OpticalFlow,
	};

	enum class DataCaptureStatus : uint8_t {
		Pending,
		Ready,
		Partial,
		Unsupported,
		Invalid,
		Failed,
		Cancelled,
	};
	enum class DataCaptureScalar : uint8_t { Float16, Float32, UInt32, UNorm8, UNorm10A2, Unknown };
	enum class DataCaptureColourSpace : uint8_t { Linear, SRGB, NotApplicable, Unknown };
	enum class DataCaptureOrigin : uint8_t { TopLeft };
	enum class DataCaptureTemporalHistory : uint8_t { Preserve, Reset, Disable };
	inline core::Name DataCaptureNode(core::Name base, DataCaptureChannel channel) {
		const std::string_view suffix = channel == DataCaptureChannel::PbrAlbedo	 ? "-albedo"
										: channel == DataCaptureChannel::PbrMaterial ? "-material"
										: channel == DataCaptureChannel::PbrEmissive ? "-emissive"
										: channel == DataCaptureChannel::MeshUv		 ? "-mesh-uv"
										: channel == DataCaptureChannel::AmbientOcclusion
											? "-ambient-occlusion"
										: channel == DataCaptureChannel::ObjectIds	  ? "-object-ids"
										: channel == DataCaptureChannel::SemanticMask ? "-semantic-ids"
										: channel == DataCaptureChannel::PartMask	  ? "-part-ids"
										: channel == DataCaptureChannel::SecondSurfaceDepth ||
												channel == DataCaptureChannel::SecondSurfaceValidity
											? "-second-surface"
											: "";
		return suffix.empty() ? base : core::Name(std::string(base.Text()) + std::string(suffix));
	}

	// These conventions are fixed by scene::ResolveCamera and are repeated on
	// every result so an exported image cannot be separated from its coordinates.
	struct DataCaptureCameraConvention {
		bool RightHandedWorld = true;
		bool CameraLooksNegativeZ = true;
		bool ClipYUp = true;
		bool DepthZeroToOne = true;
		bool ProjectionIsColumnMajor = true;
		float MetresPerWorldUnit = 1.0f;
	};

	struct DataCaptureCamera {
		// Column-major world-from-camera transform captured with the draw.
		std::array<float, 16> WorldFromCamera{};
		bool ProjectionAvailable = false;
		std::array<float, 16> Projection{};
		float VerticalFieldOfViewRadians = 0.0f;
		float NearPlaneMetres = 0.0f;
		float FarPlaneMetres = 0.0f;
		float CropLeft = 0.0f;
		float CropTop = 0.0f;
		float CropWidth = 1.0f;
		float CropHeight = 1.0f;
	};

	struct DataCaptureRequest {
		// A stable snapshot identity supplied by the world owner. It is copied
		// into every descriptor and never inferred from a renderer frame number.
		std::string SnapshotId;
		core::Name Pipeline;
		core::Name CaptureNode;
		size_t ViewSlot = 0;
		std::vector<DataCaptureChannel> Channels;
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		std::vector<DataCapturePartLabel> PartLabels;
		DataCaptureTemporalHistory TemporalHistory = DataCaptureTemporalHistory::Preserve;
	};

	struct DataCapturePlane {
		DataCaptureChannel Channel = DataCaptureChannel::RgbLinearHdr;
		DataCaptureStatus Status = DataCaptureStatus::Pending;
		core::Name Resource;
		core::Name CaptureNode;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t RowStride = 0;
		DataCaptureScalar Scalar = DataCaptureScalar::Unknown;
		DataCaptureColourSpace ColourSpace = DataCaptureColourSpace::Unknown;
		DataCaptureOrigin Origin = DataCaptureOrigin::TopLeft;
		// Present only for the ambient-occlusion plane, including an explicit
		// Unavailable state for an unrecognised R8 source.
		std::optional<AmbientOcclusionProvenance> AmbientOcclusion;
		std::string Provenance;
		// BLAKE3-256 of Bytes. It is zero until this plane is Ready.
		assets::ContentHash Hash;
		std::vector<std::byte> Bytes;
	};

	// Caller-owned ticket. The renderer only receives its resource-image tokens,
	// so cancellation and lifetime stay explicit at the product boundary.
	struct DataCaptureTicket {
		std::string SnapshotId;
		core::Name Pipeline;
		core::Name CaptureNode;
		size_t ViewSlot = 0;
		DataCaptureTemporalHistory TemporalHistory = DataCaptureTemporalHistory::Preserve;
		std::vector<DataCaptureChannel> Channels;
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		std::vector<DataCapturePartLabel> PartLabels;
		// One entry per logical channel, indexing ResourceTokens. Channels emitted
		// by one capture node share a single GPU readback.
		std::vector<uint8_t> ChannelResourceIndices;
		std::vector<uint64_t> ResourceTokens;
		bool Cancelled = false;
	};

	struct DataCapturePoll {
		DataCaptureStatus Status = DataCaptureStatus::Invalid;
		std::string SnapshotId;
		uint64_t CaptureFrame = 0;
		core::Name Pipeline;
		uint64_t PipelineRevision = 0;
		core::Name WorldName;
		size_t ViewSlot = 0;
		// This is false only for Preserve. Reset and Disable are refused until a
		// render-only pass applies their declared renderer-local history policy.
		bool TemporalHistoryChanged = false;
		DataCaptureCameraConvention Camera;
		DataCaptureCamera CameraPose;
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		std::vector<DataCapturePartLabel> PartLabels;
		std::vector<DataCapturePlane> Planes;
	};

	const char *DataCaptureChannelName(DataCaptureChannel channel);
	DataCaptureCameraConvention DataCaptureCameraConventions();
}
