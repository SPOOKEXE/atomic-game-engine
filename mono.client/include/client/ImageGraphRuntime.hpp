#pragma once

// Client host for saved image graphs published under ordinary texture names.

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/scene/ImageGraphBinding.hpp>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}
namespace engine::render {
	class Renderer;
}

namespace client {
	// One bounded host evaluation with its image or diagnostic.
	struct ImageGraphFrameResult {
		// Final parse, compile or evaluation status.
		engine::imagegraph::Status Status = engine::imagegraph::Status::Malformed;
		// Durable graph location and explanation on failure.
		engine::imagegraph::Diagnostic Diagnostic;
		// Straight RGBA8 image on success.
		engine::imagegraph::Image Image;
		// Present only when a headless Transform Image 3D export selects its mesh output.
		std::optional<engine::render::imagegraph::TransformImage3DMesh> Mesh;
		// Whether authored keyframes require evaluation at each selected tick.
		bool Animated = false;
	};

	// Resolves a checked single-segment graph stem below the asset directory.
	// Returns an empty path for unsafe or oversized names.
	std::filesystem::path
	ImageGraphDocumentPath(const std::filesystem::path &assetsDirectory, engine::core::Name graph);

	// The host reads one bounded native graph from directory/<Graph>.graph.
	// Graph is a single safe file stem, never an arbitrary path. Seed reaches
	// the evaluator request; current pure nodes are seed independent.
	ImageGraphFrameResult LoadImageGraphFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		uint64_t tick,
		uint64_t seed = 0
	);

	// Evaluates one authored graph through the renderer-owned synchronous export pass.
	// This is a headless export API. Live bindings keep GPU-resident work with the frame scheduler.
	ImageGraphFrameResult LoadImageGraphRenderExportFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		engine::render::Renderer &renderer,
		uint64_t tick,
		uint64_t seed = 0
	);

	// Owns live publication generations for one client presentation loop.
	class ImageGraphRuntime {
	  public:
		static constexpr size_t MAXIMUM_CHECKS_PER_FRAME = 8;
		static constexpr size_t MAXIMUM_CACHED_DOCUMENTS = 8;
		static constexpr size_t MAXIMUM_CACHED_DOCUMENT_BYTES = 8u * 1024u * 1024u;
		static constexpr size_t MAXIMUM_SINK_REFERENCES = 16u * 1024u;

		// Starts a presentation budget shared by every world's binding scan.
		void BeginFrame();

		// Runs on the renderer thread after simulation. Published names resolve
		// through the normal owner-scoped renderer texture table. Visual work
		// deferred by the frame budget samples the current world tick when retried;
		// it does not replay omitted ticks into the renderer.
		size_t Refresh(
			engine::ecs::Store &store,
			engine::render::Renderer &renderer,
			engine::core::Name owner,
			const std::filesystem::path &directory
		);

		// Must run before renderer shutdown. Retires all owned textures.
		void Clear(engine::render::Renderer &renderer);
		// Drops textures for worlds no longer presented by this client.
		void
		RetireInactiveOwners(engine::render::Renderer &renderer, std::span<const engine::core::Name> owners);

		// Most recent host or publication failure, cleared after a successful update.
		const std::string &LastError() const {
			return Error;
		}

		// Successful host parses. A second animated tick of an unchanged graph
		// uses its cached document and plan without increasing this counter.
		uint64_t DocumentParses() const {
			return Parses;
		}
		size_t CachedDocumentCount() const {
			return Documents.size();
		}
		size_t CachedSourceBytes() const {
			return CachedDocumentBytes;
		}

	  private:
		struct Entry {
			engine::render::LiveImageBinding Publication;
			engine::ecs::Entity Entity;
			uint64_t StoreIdentity = 0;
			engine::scene::ImageGraphBinding Selector;
			uint64_t Tick = 0;
			std::filesystem::file_time_type Modified{};
			uintmax_t FileBytes = 0;
			bool Published = false;
			bool Animated = false;
		};
		struct CachedDocument {
			engine::imagegraph::Document Authored;
			engine::imagegraph::Plan Compiled;
			std::filesystem::file_time_type Modified{};
			uintmax_t FileBytes = 0;
			uint64_t LastUse = 0;
		};
		struct SinkUsage {
			std::unordered_map<uint32_t, uint8_t> Flags;
			uint64_t Revision = 0;
			uint64_t LastUse = 0;
			bool Valid = false;
		};
		struct SkyboxGroup {
			std::array<engine::core::Name, 6> Names;
			uint64_t StoreIdentity = 0;
		};

		engine::render::LiveImagePublisher Publisher;
		std::unordered_map<uint64_t, Entry> Entries;
		std::unordered_map<std::string, CachedDocument> Documents;
		std::unordered_map<uint64_t, SinkUsage> SinkUsages;
		std::unordered_map<uint32_t, SkyboxGroup> SkyboxGroups;
		std::vector<engine::core::Name> SkyboxOwners;
		std::vector<engine::core::Name> NextSkyboxOwners;
		engine::core::Name PrioritySkyboxOwner;
		size_t NextSkyboxOwner = 0;
		bool PrioritySkyboxVisited = false;
		std::unordered_map<uint32_t, size_t> NextBindingByOwner;
		size_t CachedDocumentBytes = 0;
		uint64_t CacheClock = 0;
		uint64_t Parses = 0;
		size_t ChecksRemaining = MAXIMUM_CHECKS_PER_FRAME;
		std::string Error;
	};
}
