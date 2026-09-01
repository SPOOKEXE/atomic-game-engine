#pragma once

// How far between two ticks the editor draws a world.
//
// **One line of arithmetic, in a header, because the version of it that lived
// inside `Editor::PresentWorld` was wrong for the editor's most ordinary
// state.** `Editor` needs a window, a device and a universe to construct, so
// nothing in it is reachable from a test - and a decision a test cannot reach
// is a decision that gets to be wrong for a release. `studio/Projection.hpp`
// makes the same argument about the viewport's arithmetic and for the same
// reason.
//
// ## What went wrong
//
// A world's `PreviousTransform` is written by `capture-previous`, a
// `PreSimulation` system. `World::Present` runs `PreRender` alone, so a world
// that is not being ticked never updates it - and the draw list interpolates
// *from* it. Present such a world at alpha zero and every part is drawn at
// whatever frame it was created with, which for a part the editor made is the
// identity: the origin.
//
// The editor knew this and asked the wrong question. It presented at alpha one
// when `Universe::StateOf` was not `Active` - but `Editor::SyncWorldStates`
// deliberately leaves *every* world `Active` when nothing is running, so that
// an author returning to Edit does not find their scenes marked stopped. Plain
// Edit mode is therefore: every world `Active`, `Editor::Simulate` returning
// before `Universe::Tick`, and an accumulator that never advances - alpha
// zero, and every part drawn at the origin while its selection outline, which
// reads `Transform` directly, followed the mouse.
//
// Two halves of the frame disagreeing about where something is reads as a
// renderer fault. It is not one, and this is the arithmetic that decides it.
//
// @tier L13 · client

#include <engine/core/Name.hpp>
#include <engine/render/WorldPresentation.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/world/Enums.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace studio {
	// Whether one authored emitter belongs in an editor viewport.
	//
	// Studio previews only enabled emitters placed on a PVInstance, either
	// directly or through an Attachment below one. This is presentation policy:
	// runtime worlds retain the engine's normal emitter semantics.
	bool ParticleEmitterVisibleInStudio(
		const engine::ecs::Store &store,
		engine::ecs::Entity emitter,
		const engine::effects::ParticleEmitter &settings
	);

	// Builds the filtered particle selection and its ECS-backed revision key.
	engine::render::ParticleBatchSelection StudioParticleSelection(engine::ecs::Store &store);

	// Collects the particle snapshot shown by Studio, or clears the retained
	// snapshot when the global particle view is hidden.
	size_t CollectStudioParticleBatches(
		engine::ecs::Store &store, engine::render::ParticleFrame &frame, bool renderingEnabled
	);

	// Advances the existing resident particle system for an authored Edit-mode
	// world. Running worlds already advance through their scheduler, and a global
	// visibility disable owes neither simulation nor renderer submission.
	bool AdvanceStudioParticlePreview(
		engine::ecs::Store &store, float delta, bool worldRunning, bool renderingEnabled
	);

	// The slowly sampled values printed in Studio's always-visible status bar.
	//
	// The source counters change every frame even when the visible scene and
	// editor state do not. Keeping the displayed snapshot stable between sample
	// deadlines prevents diagnostic text from invalidating the retained Studio
	// interface and every composition above it at an uncapped render rate.
	struct StatusBarSnapshot {
		// Sampling deadline, source viewport, visible counters, and validity.
		//@{
		double NextSample = 0.0;
		size_t Viewport = 0;
		uint32_t FramesPerSecond = 0;
		uint32_t DrawCalls = 0;
		uint64_t Triangles = 0;
		uint32_t Culled = 0;
		bool Valid = false;
		//@}

		// Refreshes the visible values when their deadline expires or the
		// focused viewport changes. Returns whether the snapshot was sampled.
		bool Refresh(
			double now,
			size_t viewport,
			uint32_t framesPerSecond,
			uint32_t drawCalls,
			uint64_t triangles,
			uint32_t culled
		);
	};

	// The presentation ceilings configured by Studio.
	struct PresentationRates {
		// Interface and renderer ceilings for active and inactive states.
		//@{
		float InterfaceActive = 0.0f;
		float InterfaceIdle = 0.0f;
		float RendererFocused = 0.0f;
		float RendererUnfocused = 0.0f;
		//@}

		// Whether all four ceilings are bypassed.
		bool Uncapped = false;
	};

	// Resolves the one ceiling applied to the joined simulation and rendering
	// loop. A running world is visually active even when nobody is touching the
	// editor, so input-idle pacing applies only while editing a stopped scene.
	float
	PresentationCeiling(const PresentationRates &rates, bool focused, bool worldRunning, bool inputIdle);

	// Which alpha to present a world at.
	//
	// **Both arguments are needed and neither implies the other.** A world's
	// state says whether the driver *would* advance it; `advancing` says
	// whether the driver is being run at all. A suspended world in a ticking
	// universe and an active world in a host that has stopped ticking are both
	// standing still, and both have to be drawn at one.
	//
	// @param advancing   Whether the caller is ticking the universe this frame.
	//                    `Editor::Simulate` returns early in two cases - nothing
	//                    is running, and everything running is paused - and both
	//                    are this being `false`.
	// @param state       What the universe says about this world.
	// @param accumulator Where the world's clock is between two ticks, from
	//                    `Universe::AlphaOf`. Used only when the world is really
	//                    being advanced; it is stale otherwise, which is the
	//                    whole bug.
	// @return `accumulator` for a world that is being ticked, and `1.0f` - draw
	//         the current transform, there is nothing to interpolate towards -
	//         for one that is not.
	// @since v0.11
	float PresentationAlpha(bool advancing, engine::world::WorldState state, float accumulator);

	// Builds one world selector row.
	//
	// `active` means the world is being advanced now. It is deliberately not
	// the editor's selected world and not merely `WorldState::Active`, since edit
	// worlds retain that state while no run exists.
	std::string WorldSelectorLabel(std::string_view name, bool active);

	// Appends a replica's client-local rows to the authority scene Studio has
	// already copied for a hosted client viewport.
	//
	// Authority rows define shared visual state. Rows in the authoritative ECS
	// identity range are interpolation copies and are omitted; predicted rows
	// are client-local and are appended under the replica's world name so their
	// entity ids cannot collide with the authority's resident slots.
	//
	// The ECS identity range makes this a linear scan with no entity hash table:
	// every authoritative or anonymous row is already represented by the
	// authority scene and only predicted identities belong to the client.
	//
	// @since v0.19
	void AppendReplicaVisualInstances(
		engine::core::Name replicaWorld,
		std::span<const engine::scene::DrawInstance> replica,
		std::vector<engine::scene::DrawInstance> &authority,
		std::span<const engine::core::CFrame> replicaJoints = {},
		std::vector<engine::core::CFrame> *authorityJoints = nullptr
	);
}
