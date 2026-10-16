#pragma once

// Shared per-world CPU procedural queue. Facts remain in ECS authority.
// @tier L9 · shared
#include <engine/script/ProceduralInputs.hpp>
#include <engine/script/ProceduralScene.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace engine::script {
	struct ServiceSurface;
	// Selected by the host, never by a script request. Editor preview uses a
	// separate store from authoritative play so async readiness cannot steer ticks.
	enum class ProceduralWorldMode : uint8_t { AuthoritativeSerial, StudioPreview };
	// Owned CPU progress snapshot. Reading never publishes or waits for work.
	struct ProceduralServiceProgress {
		ProceduralProgress Work{};			 // Monotonic kernel counters and lifecycle.
		procedural::Diagnostic Diagnostic{}; // Bounded terminal diagnostic.
		procedural::Digest ArtifactDigest{}; // Set only after successful generation.
	};
	// One actual runtime's revocable endpoint. All CPU/ECS operations require the
	// current store owner thread. Detach alone is thread-safe and touches no ECS.
	class ProceduralEndpoint {
	  public:
		~ProceduralEndpoint();
		ProceduralEndpoint(const ProceduralEndpoint &) = delete;
		ProceduralEndpoint &operator=(const ProceduralEndpoint &) = delete;
		bool Configure(
			ecs::Store &,
			const procedural::Recipe &,
			std::span<const procedural::RegionScope>,
			std::span<const ProceduralSource>,
			procedural::Diagnostic &
		);
		bool SetSource(ecs::Store &, const ProceduralSource &, procedural::Diagnostic &);
		bool RemoveSource(ecs::Store &, procedural::SourceKind, std::string_view, procedural::Diagnostic &);
		bool SetSpline(ecs::Store &, const procedural::AuthoredSpline &, procedural::Diagnostic &);
		bool RemoveSpline(ecs::Store &, std::string_view, procedural::Diagnostic &);
		bool SetOverride(ecs::Store &, const procedural::PlacementOverride &, procedural::Diagnostic &);
		bool RemoveOverride(ecs::Store &, std::string_view, procedural::Diagnostic &);
		// Captures targets and inputs before admission. Accepted work binds both
		// scene targets; admission refusal leaves them unchanged.
		bool Generate(
			ecs::Store &,
			procedural::RegionScope,
			ecs::Entity mesh,
			ecs::Entity part,
			uint64_t &ticket,
			procedural::Diagnostic &
		);
		bool Progress(ecs::Store &, uint64_t ticket, ProceduralServiceProgress &, procedural::Diagnostic &);
		bool Cancel(ecs::Store &, uint64_t ticket, procedural::Diagnostic &);
		// Uses the current fixed tick and atomically publishes caller-owned tickets.
		bool Publish(ecs::Store &, std::span<const uint64_t> tickets, procedural::Diagnostic &);
		// Releases only this caller's ticket. Active cancellation stays charged until observed.
		bool Release(ecs::Store &, uint64_t ticket, procedural::Diagnostic &);
		// Neutral VM dispatch verifies the actual caller identity before forwarding.
		bool BelongsTo(const ecs::Store &, uint64_t runtimeIdentity) const;
		void Detach() noexcept;

	  private:
		struct State;
		std::unique_ptr<State> Held;
		explicit ProceduralEndpoint(std::unique_ptr<State>);
		friend std::shared_ptr<ProceduralEndpoint>
		OpenProceduralEndpoint(ecs::Store &, uint64_t, ProceduralWorldMode, procedural::Diagnostic &);
	};
	// Registers the transient queue-owner resource before the registry seals.
	void RegisterProceduralServiceComponents();
	// Creates or reuses the store's single queue. Runtime identity must be genuine,
	// nonzero and unique among attached endpoints. Opposite mode requests refuse.
	std::shared_ptr<ProceduralEndpoint> OpenProceduralEndpoint(
		ecs::Store &, uint64_t runtimeIdentity, ProceduralWorldMode, procedural::Diagnostic &
	);
	// One neutral method catalogue consumed by both interpreter adapters.
	const ServiceSurface &ProceduralServiceSurface();
}
