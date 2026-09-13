#pragma once

// A product-installed bridge from the VM-neutral script surface to an L12
// renderer. Script owns no renderer object or ticket state.
// @tier L9 · shared

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine::script {
	struct DataCaptureBridgeRequest {
		std::string InstanceId;
		std::string SnapshotId;
		std::string Pipeline;
		std::string CaptureNode;
		uint64_t ViewSlot = 0;
		std::vector<std::string> Channels;
		std::string TemporalHistory;
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
		std::string Scalar;
		std::string ColourSpace;
		std::string Origin;
		// Exact source storage for packed channels, such as rgba8_unorm.
		std::string Packing;
	};

	struct DataCaptureBridgePoll {
		std::string Status;
		std::string SnapshotId;
		uint64_t CaptureFrame = 0;

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
	};

	struct DataCaptureBridgeCapabilities {
		bool Available = false;
		std::vector<std::string> Channels;
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
	};

}
