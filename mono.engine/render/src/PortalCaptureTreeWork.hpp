#pragma once

#include <engine/render/PortalCaptureTreeCompose.hpp>
#include <engine/render/PortalCaptureTreeImport.hpp>
#include <engine/render/Renderer.hpp>

struct SDL_GPUCommandBuffer;
struct SDL_GPUFence;

namespace engine::render {
	struct PortalTreeNodeWork {
		std::vector<scene::DrawInstance> Body, ApertureRows;
		std::vector<core::CFrame> ApertureJoints;
		std::vector<PortalView> Apertures;
		PortalImageBinding Output;
		uint64_t Image = 0;
	};

	// One outstanding shadow upload prevents hidden backend allocations accumulating per node.
	struct PortalTreeCompositionJob {
		uint64_t Token = 0, Tree = 0, ResourceEpoch = 0, Prepared = 0;
		std::array<PortalTreeNodeWork, MAX_PORTAL_CAPTURE_TREE_NODES> Work;
		std::vector<core::CFrame> Joints;
		// Root-primary selection is owned across source waits; children keep every body row.
		std::vector<uint32_t> EyeHiddenRows;
		uint64_t EyeRig = 0;
		std::optional<int64_t> EyePlayer;
		std::vector<WorldContentOwner> ContentOwners;
		core::Name ContentOwner;
		size_t NextNode = 0;
		size_t OwnedBytes = 0;
		PortalTreeCompositionStatus AfterFence = PortalTreeCompositionStatus::Pending;
		bool Submitted = false, Cancelled = false;
		uint64_t Shadow = 0;
		SDL_GPUFence *Fence = nullptr;
		std::unique_ptr<PortalShadowAssembly> ShadowAssembly;
		std::optional<PortalShadowSnapshot> ShadowManifest;
		std::optional<PortalShadowImage> AssembledShadow;
		size_t ShadowAssemblyBytes = 0;
		uint32_t ShadowAssemblyNode = 0;
	};
	struct PortalPreparedTree {
		uint64_t Token = 0, Tree = 0, Lease = 0;
		std::array<uint64_t, MAX_PORTAL_CAPTURE_TREE_NODES> Shadows{};
		std::array<std::optional<core::AABB>, MAX_PORTAL_CAPTURE_TREE_NODES> BodyBounds{};
		size_t NextNode = 0;
		// Packed maps share one ordered submission. Their buffers and lease remain
		// owned until this fence completes, so a pose never waits between nodes.
		SDL_GPUCommandBuffer *Command = nullptr;
		bool Cancelled = false, PendingUpload = false, Submitted = false, UploadConfirmed = false;
		SDL_GPUFence *Fence = nullptr;
		std::unique_ptr<PortalShadowAssembly> ShadowAssembly;
		std::optional<PortalShadowSnapshot> ShadowManifest;
		std::optional<PortalShadowImage> AssembledShadow;
		size_t ShadowAssemblyBytes = 0;
		uint32_t ShadowAssemblyNode = 0;
	};

}
