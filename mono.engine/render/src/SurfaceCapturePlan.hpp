#pragma once

#include <engine/render/Renderer.hpp>
#include <engine/scene/ActiveCamera.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::render {

	// Indices identify copied inputs inside one view only. No world identity or
	// GPU handle enters the camera plan. Children execute before their parent.
	enum class SurfaceCaptureKind : uint8_t { Mirror, Portal };
	enum class SurfaceCaptureStatus : uint8_t { Ok, Invalid, BudgetExceeded };
	inline constexpr uint16_t NO_SURFACE_CAPTURE = UINT16_MAX;
	inline constexpr size_t MAX_SURFACE_CAPTURES = 512;

	struct SurfaceCaptureEntry {
		core::CFrame Frame;
		scene::CameraMatrices Matrices;
		std::array<uint16_t, scene::MAX_SURFACES> Children;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t Depth = 0;
		uint16_t Source = 0;
		int16_t Slot = -1;
		int16_t Arrival = -1;
		int16_t RootSlot = -1;
		SurfaceCaptureKind Kind = SurfaceCaptureKind::Mirror;
		uint8_t Padding{};
	};

	struct SurfaceCapturePlan {
		std::vector<SurfaceCaptureEntry> Entries;
		std::vector<uint16_t> Postorder;
		std::array<uint16_t, scene::MAX_SURFACES> Roots;
		uint64_t Pixels = 0;
	};

	struct SurfaceCaptureRequest {
		std::span<const SurfaceView> Mirrors;
		std::span<const PortalView> Portals;
		core::CFrame Frame;
		glm::mat4 Projection{1};
		uint64_t PixelBudget = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint32_t Depth = 0;
	};

	struct CaptureBlendSlot {
		uint32_t Slot = 0;
		uint32_t Source = 0;
		float DistanceSquared = 0;
	};

	// Sort packed slots, not copied scene rows. Equal distances keep authored
	// source order. Retained scratch and an explicit tie key avoid stable-sort allocation.
	bool OrderCaptureTransparency(
		std::span<const scene::DrawInstance> instances,
		std::span<const uint32_t> packed,
		uint32_t first,
		uint32_t count,
		const core::Vector3 &eye,
		std::vector<CaptureBlendSlot> &output
	);

	// Failure clears the partial plan before any target can be allocated. The
	// entry ceiling also bounds work for tiny images with many visible apertures.
	SurfaceCaptureStatus PlanSurfaceCaptures(const SurfaceCaptureRequest &request, SurfaceCapturePlan &plan);
}
