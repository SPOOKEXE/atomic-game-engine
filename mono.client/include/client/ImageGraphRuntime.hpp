#pragma once

// Client host for saved image graphs published under ordinary texture names.

#include <engine/core/Name.hpp>
#include <engine/ecs/Entity.hpp>
#include <engine/imagegraph/ComposerLuaHost.hpp>
#include <engine/imagegraph/Document.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/PendingHostObservations.hpp>
#include <engine/imagegraph/SourceArgumentHost.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>
#include <engine/render/ImageGraphTransform3D.hpp>
#include <engine/render/LiveImagePublisher.hpp>
#include <engine/render/SourceSkyboxGroup.hpp>
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
		// Owned numeric image on success, with its surface format preserved.
		engine::imagegraph::Image Image;
		uint8_t FlipbookSide = 0;
		std::vector<float> FrameDurations;
		// Present only when a headless Transform Image 3D export selects its mesh
		// output.
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
		uint64_t seed = 0,
		engine::imagegraph::SourceArgumentHost *arguments = nullptr,
		const engine::imagegraphfont::GraphFontInputs *fonts = nullptr,
		const engine::imagegraph::EvaluationRequest *fontInputs = nullptr
	);

	// Evaluates one authored graph through the renderer-owned synchronous export
	// pass. This is a headless export API. Live bindings keep GPU-resident work
	// with the frame scheduler.
	ImageGraphFrameResult LoadImageGraphRenderExportFrame(
		const std::filesystem::path &directory,
		engine::core::Name graph,
		engine::core::Name output,
		engine::render::Renderer &renderer,
		uint64_t tick,
		uint64_t seed = 0,
		engine::imagegraph::SourceArgumentHost *arguments = nullptr,
		const engine::imagegraphfont::GraphFontInputs *fonts = nullptr,
		const engine::imagegraph::EvaluationRequest *fontInputs = nullptr
	);

	// Owns live publication generations for one client presentation loop.
	class ImageGraphRuntime {
	  public:
		static constexpr size_t MAXIMUM_CHECKS_PER_FRAME = 8;
		static constexpr size_t MAXIMUM_CACHED_DOCUMENTS = 8;
		static constexpr size_t MAXIMUM_CACHED_DOCUMENT_BYTES = 8u * 1024u * 1024u;
		static constexpr size_t MAXIMUM_SINK_REFERENCES = 16u * 1024u;

		// Replaces copied arguments on the renderer thread and retires cached observations only on success.
		engine::imagegraph::Status PrepareArguments(
			const engine::imagegraph::SourceArgumentOptions &,
			engine::render::Renderer &,
			engine::imagegraph::Diagnostic &,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		);
		uint64_t ArgumentGeneration() const {
			return ArgumentsGeneration;
		}

		// Retires font-derived cache entries only after an owned configuration replacement succeeds.
		engine::imagegraph::Status PrepareFonts(
			const engine::imagegraphfont::GraphFontConfiguration &,
			const engine::assets::ContentPolicy &,
			engine::render::Renderer &,
			engine::imagegraph::Diagnostic &,
			uint64_t maximumBytes = engine::imagegraph::Limits::MaximumEvaluationBytes
		);
		uint64_t FontGeneration() const {
			return Fonts.Revision();
		}

		// Starts a presentation budget shared by every world's binding scan.
		void BeginFrame();

		// Scans current ECS bindings when their version, row count, root or cached
		// documents change. An unchanged scan appends nothing. Refusal leaves the
		// stamp unchanged; already appended names remain requestable.
		bool CollectWantedComposerShaders(
			engine::ecs::Store &store,
			engine::core::Name owner,
			const std::filesystem::path &directory,
			std::vector<engine::core::Name> &output
		);

		// Runs on the renderer thread after simulation. Successful synchronous
		// publications and admitted Transform Image 3D jobs count toward the return
		// value. Transform outputs become visible only after renderer fence adoption.
		// Visual work deferred by the frame budget samples the current world tick
		// when retried; it does not replay omitted ticks into the renderer.
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
		engine::imagegraph::SourceArgumentHost Arguments;
		engine::imagegraphfont::GraphFontInputs Fonts;
		uint64_t ArgumentsGeneration = 1;
		struct Entry {
			engine::imagegraph::CapturedFeedbackHost Feedback;
			std::unique_ptr<engine::imagegraph::ComposerLuaHost> LuaHost;
			engine::render::LiveImageBinding Publication;
			engine::core::Name Owner;
			engine::ecs::Entity Entity;
			uint64_t StoreIdentity = 0;
			engine::scene::ImageGraphBinding Selector;
			uint64_t Tick = 0;
			std::filesystem::file_time_type Modified{};
			uintmax_t FileBytes = 0;
			bool Published = false;
			bool Animated = false;
			bool TransformAdmitted = false;
			uint64_t TransformGeneration = 0;
			uint64_t ComposerRevision = 0;
			std::optional<uint64_t> PendingHostTick;
			std::vector<engine::core::Name> HostCaptures;
			engine::imagegraph::PendingHostObservations HostObservations;
		};
		struct CachedDocument {
			engine::imagegraph::Document Authored;
			engine::imagegraph::Plan Compiled;
			std::filesystem::file_time_type Modified{};
			uintmax_t FileBytes = 0;
			uint64_t LastUse = 0;
		};
		struct ComposerDemandScan {
			uint64_t StoreIdentity = 0;
			engine::core::Name Owner;
			std::filesystem::path Directory;
			uint64_t BindingVersion = 0;
			size_t BindingRows = 0;
			uint64_t DocumentRevision = 0;
		};
		std::array<ComposerDemandScan, 64> ComposerDemandScans;
		uint64_t ComposerDocumentRevision = 1;
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

		struct PendingSkyboxGroup {
			engine::render::imagegraph::SourceSkyboxGroup Work;
			std::optional<engine::render::imagegraph::SourceSkyboxRequest> Admission;
			size_t PreparedFaces = 0;
			uint64_t RetainedFaceBytes = 0, OutputFaceBytes = 0;
			std::array<engine::scene::ImageGraphBinding, 6> Selectors;
			std::array<engine::ecs::Entity, 6> Entities;
			std::array<std::filesystem::file_time_type, 6> Modified;
			std::array<uintmax_t, 6> FileBytes{};
			std::array<uint64_t, 6> Ticks{};
			std::array<bool, 6> Animated{};
			uint64_t StoreIdentity = 0;
		};
		std::unordered_map<uint32_t, PendingSkyboxGroup> PendingSkyboxes;
		engine::render::LiveImagePublisher Publisher;
		std::unordered_map<uint64_t, Entry> Entries;
		std::unordered_map<std::string, CachedDocument> Documents;
		std::unordered_map<uint64_t, SinkUsage> SinkUsages;
		std::unordered_map<uint32_t, SkyboxGroup> SkyboxGroups;
		std::unordered_map<uint32_t, engine::core::Name> TransformOwners;
		std::vector<engine::core::Name> SkyboxOwners;
		std::vector<engine::core::Name> NextSkyboxOwners;
		engine::core::Name PrioritySkyboxOwner;
		size_t NextSkyboxOwner = 0;
		bool PrioritySkyboxVisited = false;
		std::unordered_map<uint32_t, size_t> NextBindingByOwner;
		size_t CachedDocumentBytes = 0;
		uint64_t CacheClock = 0;
		uint64_t Parses = 0;
		uint64_t NextTransformGeneration = 1;
		size_t ChecksRemaining = MAXIMUM_CHECKS_PER_FRAME;
		std::string Error;
	};
} // namespace client
