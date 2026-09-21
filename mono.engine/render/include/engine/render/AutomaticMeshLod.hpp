#pragma once

// Automatic mesh LOD artifacts built from authored scene policy and published
// through renderer residency.
//
// @tier L12 · client

#include <engine/assets/MeshDecimate.hpp>
#include <engine/core/Name.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/world/Universe.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::render {
	class Renderer;

	// One derived mesh level and the worlds that requested it.
	struct AutomaticMeshLodArtifact {
		// Content name shared by matching source meshes.
		core::Name Name;
		// Host-side mesh bytes awaiting renderer upload.
		assets::MeshData Data;
		// Worlds that should retain this derived level.
		std::vector<world::WorldId> Worlds;
	};

	// Builds every missing automatic level requested by the selected worlds.
	//
	// Matching inputs share one artifact. This function only reads world state
	// and builds host data, so callers can test planning without a GPU.
	std::vector<AutomaticMeshLodArtifact>
	BuildAutomaticMeshLods(ecs::Store &store, const core::Name &base, const assets::MeshData &mesh);

	std::vector<AutomaticMeshLodArtifact> BuildAutomaticMeshLods(
		world::Universe &universe,
		std::span<const world::WorldId> worlds,
		const core::Name &base,
		const assets::MeshData &mesh
	);

	// Uploads a built batch and records each admitted artifact in its worlds.
	//
	// Returns the number of artifacts admitted by at least one content owner.
	size_t PublishAutomaticMeshLods(
		world::Universe &universe,
		Renderer &renderer,
		std::span<const core::Name> owners,
		std::span<const AutomaticMeshLodArtifact> artifacts
	);

	class AutomaticMeshLodUploader {
	  public:
		size_t
		Refresh(ecs::Store &store, Renderer &renderer, core::Name owner = {}, bool includeEditable = true);
		size_t RefreshSource(
			ecs::Store &store,
			Renderer &renderer,
			const core::Name &base,
			const assets::MeshData &mesh,
			core::Name owner = {}
		);
		void ForgetWorld(uint64_t identity);
		void ForgetOwner(core::Name owner);

	  private:
		struct Request {
			core::Name Name;
			float Ratio = 0.0f;
			scene::LodStrategy Strategy = scene::LodStrategy::None;
			bool operator==(const Request &) const = default;
		};
		struct Job {
			~Job();

			uint64_t World = 0;
			core::Name Owner;
			core::Name Base;
			uint64_t Generation = 0;
			std::mutex Guard;
			std::vector<AutomaticMeshLodArtifact> Result;
			bool Ready = false;
			std::atomic_bool CancelRequested = false;
			std::thread Worker;

			void RequestCancel() {
				CancelRequested.store(true, std::memory_order_relaxed);
			}
		};
		struct Queued {
			uint64_t Ticket = 0;
			uint64_t Generation = 0;
			assets::MeshData Mesh;
			std::vector<Request> Requests;
		};
		struct Source {
			core::Name Base;
			std::vector<core::Name> Artifacts;
			std::vector<Request> Requested;
			bool Changed = true;
			uint64_t Generation = 0;
			std::unique_ptr<Queued> Next;
		};
		struct Scope {
			uint64_t World = 0;
			core::Name Owner;
			std::vector<Source> Sources;
		};
		static std::vector<Request> Requests(ecs::Store &store, core::Name base);
		static std::vector<AutomaticMeshLodArtifact> Build(
			const assets::MeshData &mesh,
			std::span<const Request> requests,
			assets::MeshDecimationCancelToken cancel
		);
		void Start(uint64_t world, core::Name owner, Source &source);
		size_t Collect(ecs::Store &store, Renderer &renderer);
		size_t RefreshStored(
			ecs::Store &store,
			Renderer &renderer,
			Source &source,
			const std::vector<Request> &wanted,
			core::Name owner,
			const assets::MeshData *provided = nullptr
		);
		bool ResolveSource(
			ecs::Store &store,
			Renderer &renderer,
			const Source &source,
			core::Name owner,
			assets::MeshData &out
		);
		bool RetainsArtifact(core::Name owner, core::Name artifact) const;
		void ReleaseArtifacts(
			ecs::Store &store, Renderer &renderer, core::Name owner, std::span<const core::Name> artifacts
		);
		std::vector<Scope> Scopes;
		std::unique_ptr<Job> Active;
		std::vector<std::unique_ptr<Job>> Completed;
		uint64_t NextTicket = 0;
	};
}
