#pragma once

// Packs completed capture planes into an explicitly mapped RGBA32F image.
// This stays at the bridge boundary: it does not create a graph resource or
// alter the native planes retained by the renderer.

#include <engine/render/DataCapture.hpp>
#include <engine/script/DataCaptureBridge.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace engine::render::data_capture_packing {
	inline constexpr size_t MAX_PACKED_BYTES = 64 * 1024 * 1024;
	struct Result {
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<std::byte> Bytes;
	};

	inline float Float16ToFloat32(uint16_t bits) {
		const uint32_t sign = static_cast<uint32_t>(bits & 0x8000) << 16;
		uint32_t exponent = (bits >> 10) & 0x1f;
		uint32_t fraction = bits & 0x03ff;
		if (exponent == 0) {
			if (fraction == 0) return std::bit_cast<float>(sign);
			int32_t subnormalExponent = -14;
			while ((fraction & 0x0400) == 0) {
				fraction <<= 1;
				--subnormalExponent;
			}
			fraction &= 0x03ff;
			exponent = static_cast<uint32_t>(subnormalExponent + 127);
		} else if (exponent == 0x1f) {
			return std::bit_cast<float>(sign | 0x7f800000 | (fraction << 13));
		} else {
			exponent += 127 - 15;
		}
		return std::bit_cast<float>(sign | (exponent << 23) | (fraction << 13));
	}

	inline size_t ComponentCount(const DataCapturePlane &plane) {
		switch (plane.Channel) {
		case DataCaptureChannel::RgbLinearHdr:
		case DataCaptureChannel::PbrAlbedo:
		case DataCaptureChannel::PbrMaterial:
		case DataCaptureChannel::PbrEmissive:
		case DataCaptureChannel::PackedGpu:
			return 4;
		case DataCaptureChannel::MeshUv:
		case DataCaptureChannel::MotionVectors:
			return 2;
		case DataCaptureChannel::LinearDepth:
		case DataCaptureChannel::AmbientOcclusion:
		case DataCaptureChannel::FirstSurfaceValidity:
		case DataCaptureChannel::SecondSurfaceDepth:
		case DataCaptureChannel::SecondSurfaceValidity:
			return 1;
		default:
			return 0;
		}
	}

	inline size_t ComponentBytes(DataCaptureScalar scalar) {
		return scalar == DataCaptureScalar::UNorm8 ? 1
			 : scalar == DataCaptureScalar::Float16 ? 2
		 : scalar == DataCaptureScalar::Float32 ? 4
											 : 0;
	}

	inline bool PixelComponent(
		const DataCapturePlane &plane, uint32_t column, uint32_t row, uint8_t component, float &value
	) {
		if (column >= plane.Width || row >= plane.Height || plane.Width == 0 || plane.Height == 0) return false;
		const size_t pixelBytes = ComponentCount(plane) * ComponentBytes(plane.Scalar);
		const size_t rowOffset = static_cast<size_t>(row) * plane.RowStride;
		const size_t pixelOffset = rowOffset + static_cast<size_t>(column) * pixelBytes;
		if (pixelBytes == 0 || plane.RowStride < static_cast<size_t>(plane.Width) * pixelBytes ||
			rowOffset > plane.Bytes.size() ||
			pixelBytes > plane.Bytes.size() - rowOffset || pixelOffset > plane.Bytes.size() ||
			pixelBytes > plane.Bytes.size() - pixelOffset)
			return false;
		switch (plane.Scalar) {
		case DataCaptureScalar::UNorm8:
			if (component >= pixelBytes) return false;
			value = static_cast<float>(std::to_integer<uint8_t>(plane.Bytes[pixelOffset + component])) / 255.0f;
			return true;
		case DataCaptureScalar::Float16: {
			if (component >= pixelBytes / 2 || pixelBytes % 2 != 0) return false;
			uint16_t bits = 0;
			std::memcpy(&bits, plane.Bytes.data() + pixelOffset + static_cast<size_t>(component) * 2, sizeof(bits));
			value = Float16ToFloat32(bits);
			return true;
		}
		case DataCaptureScalar::Float32: {
			if (component >= pixelBytes / 4 || pixelBytes % 4 != 0) return false;
			std::memcpy(
				&value, plane.Bytes.data() + pixelOffset + static_cast<size_t>(component) * 4, sizeof(value)
			);
			return true;
		}
		default:
			return false;
		}
	}

	inline bool PackRgba32F(
		const std::array<const DataCapturePlane *, 4> &planes,
		const std::array<script::DataCaptureBridgePackedComponent, 4> &components,
		Result &result,
		std::string &rejection
	) {
		result = {};
		for (const DataCapturePlane *plane : planes)
			if (plane == nullptr || plane->Status != DataCaptureStatus::Ready || plane->Width == 0 ||
				plane->Height == 0 || plane->Bytes.empty()) {
				rejection = "source_plane_unavailable";
				return false;
			}
		// Output dimensions always come from R. This makes the selected depth lane
		// the calibration-preserving reference in depth/AO/material captures.
		const DataCapturePlane &output = *planes.front();
		if (output.Width > UINT32_MAX / 16 ||
			static_cast<size_t>(output.Width) * output.Height > SIZE_MAX / 16 ||
			static_cast<size_t>(output.Width) * output.Height * 16 > MAX_PACKED_BYTES) {
			rejection = "packed_plane_size_overflow";
			return false;
		}
		result.Width = output.Width;
		result.Height = output.Height;
		result.Bytes.resize(static_cast<size_t>(result.Width) * result.Height * 16);
		for (uint32_t row = 0; row < result.Height; ++row)
			for (uint32_t column = 0; column < result.Width; ++column)
				for (size_t lane = 0; lane < components.size(); ++lane) {
					const DataCapturePlane &source = *planes[lane];
					const uint32_t sourceColumn = static_cast<uint32_t>(
						(static_cast<uint64_t>(column) * 2 + 1) * source.Width / (result.Width * 2)
					);
					const uint32_t sourceRow = static_cast<uint32_t>(
						(static_cast<uint64_t>(row) * 2 + 1) * source.Height / (result.Height * 2)
					);
					float value = 0.0f;
					if (!PixelComponent(source, sourceColumn, sourceRow, components[lane].SourceComponent, value)) {
						rejection = "unsupported_source_layout";
						result = {};
						return false;
					}
					std::memcpy(
						result.Bytes.data() + (static_cast<size_t>(row) * result.Width + column) * 16 + lane * 4,
						&value,
						sizeof(value)
					);
				}
		return true;
	}
}
