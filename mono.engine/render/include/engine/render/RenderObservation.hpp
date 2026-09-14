#pragma once

#include <engine/core/Name.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace engine::render {
	// Stable hook kinds used by capture manifests and capability discovery. The
	// enum stays process-local. Persisted records use RenderObservationHookName.
	enum class RenderObservationHook : uint8_t { DataCapture };

	constexpr std::string_view RenderObservationHookName(RenderObservationHook hook) {
		switch (hook) {
		case RenderObservationHook::DataCapture:
			return "data_capture";
		}
		return {};
	}

	inline constexpr size_t MAX_RENDER_OBSERVATION_RESOURCES = 6;

	// Camera facts copied at a graph observation point. This is a value record,
	// so an asynchronous readback never observes a later camera or view.
	struct RenderObservationCamera {
		std::array<float, 16> WorldFromCamera{};
		bool ProjectionAvailable = false;
		std::array<float, 16> Projection{};
		float FieldOfViewRadians = 0.0f;
		float NearPlane = 0.0f;
		float FarPlane = 0.0f;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	// An immutable-by-ownership record of one render observation. It names graph
	// resources, never device handles, and contains no callbacks or mutable state.
	struct RenderObservationContext {
		RenderObservationHook Hook = RenderObservationHook::DataCapture;
		core::Name Pipeline;
		core::Name Node;
		core::Name WorldName;
		size_t ViewSlot = 0;
		std::string SnapshotId;
		uint64_t Frame = 0;
		RenderObservationCamera Camera;
		std::array<core::Name, MAX_RENDER_OBSERVATION_RESOURCES> ReadResources{};
		uint8_t ReadCount = 0;
		std::array<core::Name, MAX_RENDER_OBSERVATION_RESOURCES> WriteResources{};
		uint8_t WriteCount = 0;
	};
}
