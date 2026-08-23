#pragma once

// The engine-owned boundary between a world and one rendered frame.
//
// Client and Studio both present the same scene vocabulary. The derived rows
// and graph installation rules live here so either host cannot quietly omit a
// frame input or choose a different fallback pipeline.
//
// @tier L12 · client

#include <engine/core/Name.hpp>
#include <engine/core/types/Vector3.hpp>
#include <engine/effects/ParticleSystem.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::graph {
	class PipelineSet;
}

namespace engine::ecs {
	class Store;
}

namespace engine::render {

	// What one world publishes for a presentation host to draw.
	//
	// This is derived state in the ECS. Its capacity survives between frames,
	// while serialisation deliberately writes no instances because PreRender
	// rebuilds them before use.
	struct DrawList {
		// One row per visible scene instance.
		std::vector<scene::DrawInstance> Instances;
	};

	// Renderer settings that affect scene pixels without changing a draw row.
	struct ScenePresentationState {
		scene::WorldLighting Lighting;
		uint64_t Animation = 0;
		uint64_t Resources = 0;
		uint32_t SurfaceBounces = 0;
		uint32_t SurfaceLimit = 0;
		core::Name PostProcess;
		bool Untextured = false;
	};

	// Signs all inputs that can change the scene layer. Game and host interface
	// signatures deliberately do not enter this value.
	uint64_t ScenePresentationSignature(const View &view, const ScenePresentationState &state);

	// The same scene inputs split by resident source for cache diagnostics.
	ScenePresentationSignatures
	ScenePresentationSignaturesOf(const View &view, const ScenePresentationState &state);

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
		std::vector<effects::EmitterBlock> Blocks;
		std::vector<effects::EmitterSpawnState> SpawnStates;
		std::vector<effects::EmitterRuntime> RuntimeStates;

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
		using Predicate = bool (*)(const ecs::Store &, ecs::Entity, const effects::ParticleEmitter &);

		core::Name Name;
		Predicate Includes = nullptr;
		uint64_t Revision = 0;
	};

	// Rebuilds the world-owned draw list from visible scene rows.
	//
	// Interpolation and device-neutral draw payload construction happen once
	// here for every presentation host. The resulting resource is consumed by
	// the renderer and serialises as derived state.
	//
	// @param store The world being presented.
	void CollectInstances(ecs::Store &store);

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
	// Lights without a parent transform are skipped. Same-world portal copies
	// are included, then the result is capped to MAX_SCENE_LIGHTS nearest first.
	//
	// @param store The world being presented.
	// @param eye The camera position used for ordering.
	// @param lights Cleared and filled, preserving capacity.
	// @return The number of lights written.
	size_t CollectLights(ecs::Store &store, const core::Vector3 &eye, std::vector<SceneLight> &lights);

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
}
