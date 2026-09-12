#pragma once

// Immutable renderer capture records for data-producing clients. This is a
// renderer boundary, not a statement that every named data channel exists.

#include <engine/assets/ContentHash.hpp>
#include <engine/core/Name.hpp>
#include <engine/render/ResourceImage.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace engine::render {

	// Stable channel names are represented by this enum at the device boundary.
	// A channel only becomes Ready when its declared render-graph input was copied.
	enum class DataCaptureChannel : uint8_t {
		RgbLinearHdr,
		LinearDepth,
		ShadingNormal,
		PbrAlbedo,
		PbrMaterial,
		PbrEmissive,
		AmbientOcclusion,
		ObjectIds,
		SemanticMask,
		PartMask,
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
	enum class DataCaptureScalar : uint8_t { Float16, Float32, UNorm8, UNorm10A2, Unknown };
	enum class DataCaptureColourSpace : uint8_t { Linear, SRGB, NotApplicable, Unknown };
	enum class DataCaptureOrigin : uint8_t { TopLeft };
	enum class DataCaptureTemporalHistory : uint8_t { Preserve, Reset, Disable };

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
		std::vector<uint64_t> ResourceTokens;
		bool Cancelled = false;
	};

	struct DataCapturePoll {
		DataCaptureStatus Status = DataCaptureStatus::Invalid;
		std::string SnapshotId;
		uint64_t CaptureFrame = 0;
		// This is false only for Preserve. Reset and Disable are refused until a
		// render-only pass applies their declared renderer-local history policy.
		bool TemporalHistoryChanged = false;
		DataCaptureCameraConvention Camera;
		DataCaptureCamera CameraPose;
		std::vector<DataCapturePlane> Planes;
	};

	const char *DataCaptureChannelName(DataCaptureChannel channel);
	DataCaptureCameraConvention DataCaptureCameraConventions();
}
