#pragma once

#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/Renderer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace engine::render::test_support {
	enum class TransformImage3DQueuePhase : uint8_t { Free, Queued, Recorded, Submitted, Ready, Failed };

	struct TransformImage3DQueueSlotSnapshot {
		TransformImage3DQueuePhase Phase = TransformImage3DQueuePhase::Free;
		core::Name Owner;
		core::Name Name;
		uint64_t Generation = 0;
		uint64_t SourceBytes = 0;
		uint64_t ScratchBytes = 0;
		bool Cancelled = false;
		imagegraph::TransformImage3DRequest Request;
	};

	// Private test access exposes queue ownership and copied controls without making
	// renderer implementation state part of the public render API.
	struct TransformImage3DResidentTestAccess {
		static std::array<TransformImage3DQueueSlotSnapshot, 4> Slots(const Renderer &renderer);
		static uint64_t SourceBytes(const Renderer &renderer);
		static bool SetPhase(Renderer &renderer, size_t slot, TransformImage3DQueuePhase phase);
		static bool SetCancelled(Renderer &renderer, size_t slot, bool cancelled);
		static bool RecordAndSubmit(Renderer &renderer);
		static void Poll(Renderer &renderer);
		static void *PublishedTexture(const Renderer &renderer, core::Name owner, core::Name name);
		static uint64_t PublishedGeneration(const Renderer &renderer, core::Name owner, core::Name name);
	};
}
