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

	// Returns the stable manifest spelling for a process-local hook enum value.
	constexpr std::string_view RenderObservationHookName(RenderObservationHook hook) {
		switch (hook) {
		case RenderObservationHook::DataCapture:
			return "data_capture";
		}
		return {};
	}

	// Maximum named graph resources recorded per read or write side.
	inline constexpr size_t MAX_RENDER_OBSERVATION_RESOURCES = 6;

	// Camera facts copied at a graph observation point. This is a value record,
	// so an asynchronous readback never observes a later camera or view.
	struct RenderObservationCamera {
		// Column-major transform from camera coordinates into world coordinates.
		std::array<float, 16> WorldFromCamera{};
		// Whether Projection contains a camera projection rather than a placeholder.
		bool ProjectionAvailable = false;
		// Column-major camera projection with native zero-to-one depth.
		std::array<float, 16> Projection{};
		// Vertical camera field of view in radians.
		float FieldOfViewRadians = 0.0f;
		// Near clipping distance in world units.
		float NearPlane = 0.0f;
		// Far clipping distance in world units.
		float FarPlane = 0.0f;
		// Observed view width in display pixels.
		uint32_t Width = 0;
		// Observed view height in display pixels.
		uint32_t Height = 0;
	};

	// An immutable-by-ownership record of one render observation. It names graph
	// resources, never device handles, and contains no callbacks or mutable state.
	struct RenderObservationContext {
		// Hook kind that received this immutable graph observation.
		RenderObservationHook Hook = RenderObservationHook::DataCapture;
		// Stable pipeline name active at the observation point.
		core::Name Pipeline;
		// Stable graph node name that emitted the observation.
		core::Name Node;
		// Stable world name rather than a process-local world handle.
		core::Name WorldName;
		// Renderer view slot containing the observed world.
		size_t ViewSlot = 0;
		// Immutable world snapshot identity used by asynchronous capture consumers.
		std::string SnapshotId;
		// Renderer frame sequence number at the observation point.
		uint64_t Frame = 0;
		// Pipeline revision paired with Pipeline and Node.
		uint64_t PipelineRevision = 0;
		// Camera facts copied with the graph observation.
		RenderObservationCamera Camera;
		// Named graph resources read by Node.
		std::array<core::Name, MAX_RENDER_OBSERVATION_RESOURCES> ReadResources{};
		// Number of populated ReadResources entries.
		uint8_t ReadCount = 0;
		// Named graph resources written by Node.
		std::array<core::Name, MAX_RENDER_OBSERVATION_RESOURCES> WriteResources{};
		// Number of populated WriteResources entries.
		uint8_t WriteCount = 0;
	};
}
