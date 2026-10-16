#pragma once

// arch-waiver public-header: forward studio API. Preview adapters share this
// complete native controller contract.

// Native procedural preview ownership and explicit scene activation.
// The authoring store remains the source of truth; this store holds admitted
// immutable inputs and the last accepted preview only.

#include <engine/parallel/Jobs.hpp>
#include <engine/procedural/Region.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace engine::ecs {
	class Store;
}
namespace engine::script {
	struct ProceduralPublication;
	struct ProceduralSceneTarget;
	struct ProceduralProgress;
}

namespace studio {
	// Captures fresh ECS witnesses and hashes their canonical regional key.
	bool CurrentProceduralKey(
		const engine::ecs::Store &source,
		engine::procedural::RegionScope scope,
		engine::procedural::Digest &out,
		engine::procedural::Diagnostic &diagnostic
	);

	// Owns a bounded native job host and its isolated preview store on one thread.
	class ProceduralEditor {
	  public:
		// Threaded is the native editor path; Serial supports deterministic callers.
		explicit ProceduralEditor(
			engine::parallel::JobContext context = engine::parallel::JobContext::Threaded
		);
		// Stops and joins every job before destroying the preview store.
		~ProceduralEditor();
		ProceduralEditor(const ProceduralEditor &) = delete;
		ProceduralEditor &operator=(const ProceduralEditor &) = delete;
		ProceduralEditor(ProceduralEditor &&) = delete;
		ProceduralEditor &operator=(ProceduralEditor &&) = delete;

		// Synchronizes changed canonical inputs and preserves unaffected regional work.
		bool SyncSource(const engine::ecs::Store &source, engine::procedural::Diagnostic &diagnostic);
		// Enables submission, including after an explicit cancellation or a failure.
		void Start();
		// Polls without waiting and submits dirty scopes within the host byte ceiling.
		bool Pump(engine::procedural::Diagnostic &diagnostic);
		// Stops unpublished work and disables submission until Start is called.
		void Cancel();
		// Rechecks the source, then publishes ready regions and captured targets atomically.
		bool Activate(
			const engine::ecs::Store &source,
			uint64_t activationTick,
			engine::procedural::Diagnostic &diagnostic
		);
		// Checks current source witnesses and canonical output before copying a receipt.
		bool CaptureAccepted(
			const engine::ecs::Store &source,
			engine::procedural::RegionScope scope,
			engine::script::ProceduralPublication &out,
			engine::procedural::Diagnostic &diagnostic
		) const;
		// Owner-thread view for preview rendering and upload adapters.
		engine::ecs::Store &PreviewStore();
		// Read-only owner-thread view of accepted preview geometry.
		const engine::ecs::Store &PreviewStore() const;
		// Current regional bindings; in-flight jobs retain their own captured fences.
		std::span<const engine::script::ProceduralSceneTarget> Targets() const;
		// Last nonblocking pump's request counters, valid until the next operation.
		std::span<const engine::script::ProceduralProgress> Progress() const;
		// Whether automatic submission is enabled by Start.
		bool Running() const;
		// Includes canceled and ready work until its observed record is released.
		size_t ReservedBytes() const;

	  private:
		struct State;
		std::unique_ptr<State> Held;
	};
}
