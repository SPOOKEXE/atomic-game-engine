#pragma once

// The engine-owned boundary between a world and one rendered frame.
//
// Client and Studio both present the same scene vocabulary. The derived rows
// and graph installation rules live here so either host cannot quietly omit a
// frame input or choose a different fallback pipeline.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/core/types/AABB.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/render/DataCapture.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/scene/Skinning.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::graph {
	class PipelineSet;
}

namespace engine::ecs {
	class Store;
}

namespace engine::scene {
	struct SurfaceSlot;
}

namespace engine::render {

	// Selects the active first-person Humanoid body, including a held camera's
	// original rig. Clears both selections for other camera modes or subjects.
	void SelectFirstPersonBody(const ecs::Store &store, View &view);

	// Resolves EyePlayer against this world's current player-character links.
	// Missing or ambiguous identities clear EyeRig; held camera copies are omitted.
	void ResolveEyeBody(ecs::Store &store, View &view);

	// What one world publishes for a presentation host to draw.
	//
	// This is derived state in the ECS. Its capacity survives between frames,
	// while serialisation deliberately writes no instances because PreRender
	// rebuilds them before use.
	struct DrawList {
		// One row per visible scene instance.
		std::vector<scene::DrawInstance> Instances;
		// Stable object identities indexed by captured object-id values.
		std::vector<DataCaptureObjectLabel> ObjectLabels;
		// Stable semantic identities indexed by captured class values.
		std::vector<DataCaptureSemanticLabel> SemanticLabels;
		// Stable part identities indexed by captured part-id values.
		std::vector<DataCapturePartLabel> PartLabels;
		// False when ObjectLabels cannot describe this draw list completely.
		bool ObjectLabelsValid = true;
		// False when SemanticLabels cannot describe this draw list completely.
		bool SemanticLabelsValid = true;
		// False when PartLabels cannot describe this draw list completely.
		bool PartLabelsValid = true;

		// Joint transforms for those instances, flattened into one allocation.
		std::vector<core::CFrame> JointFrames;

		// The source-built prefix excludes portal clones and face markers. Keeping
		// it lets a quiet object layer reuse interpolation and palette work while
		// those view-derived rows are rebuilt independently.
		size_t BaseInstanceCount = 0;

		// Monotonic source epochs last inspected by CollectInstances. These are
		// derived cache state and deliberately do not cross snapshots.
		std::array<uint64_t, 14> SourceRevisions{};
		// Entity count used to size the cached source rows.
		size_t SourceEntityCount = 0;
		// Number of skeletons represented by the cached source rows.
		size_t SkeletonCount = 0;
		// Number of bones represented by the cached joint palette.
		size_t BoneCount = 0;
		// Whether source rows are ready for collection.
		bool SourcesReady = false;
		// Whether the draw list was built from interpolated tick state.
		bool HasInterpolation = false;
		// Whether any source rows use a visibility filter.
		bool HasFilteredSources = false;
	};

	// Renderer settings that affect scene pixels without changing a draw row.
	struct ScenePresentationState {
		// Lighting, animation, resource, surface, shader, and debug inputs.
		//@{
		scene::WorldLighting Lighting;
		uint64_t Animation = 0;
		uint64_t Resources = 0;
		uint32_t SurfaceBounces = 0;
		uint32_t SurfaceLimit = 0;
		core::Name PostProcess;
		bool Untextured = false;
		//@}
	};

	// Signs all inputs that can change the scene layer. Game and host interface
	// signatures deliberately do not enter this value.
	uint64_t ScenePresentationSignature(const View &view, const ScenePresentationState &state);

	// The same scene inputs split by resident source for cache diagnostics.
	ScenePresentationSignatures
	ScenePresentationSignaturesOf(const View &view, const ScenePresentationState &state);

	// Signs inputs that can move an invisible particle layer back into a camera.
	// Simulation time is deliberately absent, so an off-camera resident pool can
	// advance without invalidating pixels that remain unchanged.
	uint64_t ParticleVisibilitySignature(const View &view);

	// Whether the selected Lighting children produce an environment layer.
	// Base lighting still affects objects, but an empty Lighting service has no
	// sky, atmosphere or clouds to retain and is therefore not a cache source.
	bool EnvironmentLayerPresent(const scene::WorldLighting &lighting);

	// Signs the target geometry independently from its contents.
	uint64_t ViewportPresentationSignature(uint32_t width, uint32_t height);

	// Everything one frame of a world's particles needs.
	struct ParticleFrame {
		// One batch per emitter with a live resident block.
		std::vector<ParticleBatch> Batches;

		// Same-world portal seams applied by the device particle step.
		std::vector<ParticleSeam> Seams;

		// Number of resident particle slots required for this world.
		uint32_t Pool = 0;

		// Number of blocks the world has allocated.
		uint32_t BlockCount = 0;

		// Monotonically identifies the collected presentation state.
		uint64_t Revision = 0;

		// Monotonically identifies emitter membership and draw-state ordering.
		uint64_t LayoutRevision = 0;

		// Monotonically identifies resident block parameter and curve content.
		uint64_t ResidentRevision = 0;

		// Source identity and source revisions represented by this snapshot.
		//@{
		core::Name SourceWorld;
		uint64_t SourceRevision = 0;
		uint64_t SourceLayoutRevision = 0;
		uint64_t SourceResidentRevision = 0;
		uint64_t SourceSelectionRevision = 0;
		//@}

		// The optional emitter selection represented by this snapshot. Stored so
		// switching between a complete client frame and a filtered editor frame
		// cannot reuse the other one's batch membership. The predicate itself is
		// call-local and never crosses the world boundary.
		core::Name SourceSelection;

		// Copies of blocks used when the frame outlives the store boundary.
		//@{
		std::vector<effects::EmitterBlock> Blocks;
		std::vector<effects::EmitterSpawnState> SpawnStates;
		std::vector<effects::EmitterRuntime> RuntimeStates;
		//@}

		// Whether batch pointers already name Blocks.
		bool Detached = false;

		// Copies pointed-to blocks into this frame and repoints every batch.
		void Detach();

		// Clears frame-local arrays while preserving their capacity.
		void Clear();
	};

	// Optional membership rule for a particle presentation snapshot.
	//
	// The predicate is a plain function pointer because collection is a hot
	// boundary and owns no callable allocation. Revision must change whenever
	// state read only by the predicate changes.
	struct ParticleBatchSelection {
		// Non-owning test applied while the world is entered.
		using Predicate = bool (*)(const ecs::Store &, ecs::Entity, const effects::ParticleEmitter &);

		// Stable policy name, predicate, and caller-owned invalidation revision.
		//@{
		core::Name Name;
		Predicate Includes = nullptr;
		uint64_t Revision = 0;
		//@}
	};

	// The regular presentation packet interpolates between completed ticks. A
	// data-factory snapshot instead renders the current completed tick exactly.
	enum class DrawCollectionTime : uint8_t { Interpolated, CurrentTick };

	// Rebuilds the world-owned draw list from visible scene rows.
	//
	// Interpolation and device-neutral draw payload construction happen once
	// here for every presentation host. The resulting resource is consumed by
	// the renderer and serialises as derived state.
	//
	// @param store The world being presented.
	void CollectInstances(ecs::Store &store);
	// Rebuilds the draw list using the requested completed-tick time policy.
	void CollectInstances(ecs::Store &store, DrawCollectionTime time);

	// Rebuilds the flat joint palette and assigns each skinned draw row its run.
	// Useful to both the live-world and replicated collectors.
	void CollectSkinPalettes(ecs::Store &store, DrawList &drawList);

	// Copies the palettes referenced by copied rows into their destination and
	// rewrites every offset. An invalid source range becomes an unskinned row.
	void RebaseSkinPalettes(
		std::span<scene::DrawInstance> instances,
		std::span<const core::CFrame> source,
		std::vector<core::CFrame> &destination
	);

	// Collects a world's resident particle inputs into one frame snapshot.
	//
	// Batches initially borrow the world's blocks. Call ParticleFrame::Detach
	// before leaving the store boundary when rendering happens later.
	//
	// @param store The world being presented.
	// @param frame Cleared and filled with the complete particle input.
	// @param selection Optional emitter membership rule and its revision.
	// @return The number of live emitter batches.
	size_t CollectParticleBatches(
		ecs::Store &store, ParticleFrame &frame, const ParticleBatchSelection &selection = {}
	);

	// Collects and orders the lights relevant to one camera.
	//
	// Lights without a parent transform are skipped, then the result is capped
	// by distance from their influence volume to visible receiver bounds. Portal
	// transport is captured as a bounded seam light field by the render graph,
	// keeping a wide local lamp from multiplying into point lights at every mouth.
	// An empty receiver span falls back to the camera point for callers that do
	// not own a view.
	//
	// @param store The world being presented.
	// @param eye The camera position used when no visible receiver is supplied.
	// @param receivers Visible world-space receiver bounds used for ordering.
	// @param lights Cleared and filled, preserving capacity.
	// @return The number of lights written.
	size_t CollectLights(
		ecs::Store &store,
		const core::Vector3 &eye,
		std::span<const core::AABB> receivers,
		std::vector<SceneLight> &lights
	);

	// Convenience for collectors outside a camera view. The camera point is the
	// only available receiver, so view-aware callers should use the overload.
	inline size_t
	CollectLights(ecs::Store &store, const core::Vector3 &eye, std::vector<SceneLight> &lights) {
		return CollectLights(store, eye, {}, lights);
	}

	// Builds one renderer key for an authored profile in one world.
	//
	// A runtime pipeline is local to one world even when several worlds select
	// the same authored profile. Diagnostic and capture callers must derive this
	// key from the world they have already validated.
	//
	// @param profile The authored profile name.
	// @param world The stable world number that owns the runtime graph.
	// @return The renderer key for this profile in that one world.
	core::Name WorldPipelineKey(core::Name profile, uint64_t world);

	// Installs one universe rendering profile under a world-qualified key.
	//
	// The selected profile is tried first, followed by Default PBR and the
	// remaining authored profiles. Invalid documents are reported and skipped.
	// An invalid return selects the renderer's engine default graph.
	//
	// @param profiles The universe-authored pipeline documents.
	// @param renderer The runtime pipeline cache.
	// @param world The stable world number used to qualify the installed key.
	// @param selected The profile selected by the world.
	// @return The installed key, or an invalid name when every candidate failed.
	core::Name InstallWorldPipeline(
		const graph::PipelineSet &profiles, Renderer &renderer, uint64_t world, core::Name selected
	);

	// Registers the engine-owned presentation resources under stable names.
	//
	// Idempotent. Call before any store first asks for DrawList's component id.
	void RegisterPresentationComponents();
	// Copies shared seam geometry and traversal mappings for local portal captures.
	size_t CollectPortalViews(
		ecs::Store &store, std::vector<PortalView> &portals, std::span<const scene::SurfaceSlot> slots = {}
	);

	// Applies request-local slots to copied rows from this world only. Rows from
	// foreign presentation messages retain their independently owned indices.
	void ApplySurfaceSlots(
		std::span<scene::DrawInstance> instances, std::span<const scene::SurfaceSlot> slots, core::Name world
	);

	// Copies surface views in entity order. An explicit viewer derives mirror
	// frames through scene::ReflectCamera without changing the active camera.
	size_t CollectSurfaceViews(
		ecs::Store &store,
		std::vector<SurfaceView> &views,
		std::span<const PortalView> portals = {},
		const View *viewer = nullptr,
		std::span<const scene::SurfaceSlot> slots = {}
	);

}
