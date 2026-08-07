#pragma once

// The v0.1 demo scene.
//
// **The components are `mono.engine/scene`'s and nothing here declares one.**
// This file used to carry a `Transform`, a `PreviousTransform`, a `Visual`, a
// `SceneBounds` and an `ActiveCamera` of its own, because the ECS is storage
// and does not know what a Transform is and there was nowhere shared to put
// them. `scene` at L7 is that place, both programs register the same set under
// the same names, and a snapshot now crosses between them with no translation
// layer. What is left here is the demo: `Spin` and `Orbit`, which describe how
// this scene moves and nothing else does, and `DrawList`, which is what one
// world hands its compositor.
//
// **There is no scene object.** Building the world is a function, and
// everything the tick touches is in the store: per-entity data as components,
// world-scoped data as resources. That is not tidiness — a scene class with the
// draw list and the clock as members puts the state the renderer reads outside
// the world, where the affinity check does not cover it, the profiler does not
// see it, and a second world cannot have its own.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <cstdint>
#include <vector>

namespace client {

	// --- components: per-entity, and iterated ------------------------------
	//
	// `Orbit` and `Spin` used to be declared here, and they moved to
	// `engine::examples` at v0.5 for the reason that module's CMakeLists gives:
	// a scene is not a client-tier idea. A server authors the same world and
	// replicates it, so a component only a client could name was a component
	// only a client could ever build a scene out of. These are the same two
	// types under `examples.Orbit` and `examples.Spin`.

	using engine::examples::Orbit;
	using engine::examples::Spin;

	// --- resources: one of each, for the whole world -----------------------

	// What to draw this frame, as the *world* describes it.
	//
	// In the world rather than beside it, because the alternative is the thing
	// repo_layout.md §1 names outright: a module keeping a private vector for
	// data another module reads. The vector's capacity survives from frame to
	// frame, so a steady scene stops allocating after the first one.
	struct DrawList {
		// One entry per visible cube, rebuilt every frame.
		//
		// `scene::DrawInstance`, not a renderer's instance: a `server`-tier host
		// publishes one of these too, so the payload cannot be a type only a
		// client can name. The conversion into a matrix and an RGBA happens in
		// `render`, once, at the point of upload.
		//
		// Cleared rather than reallocated, so the capacity survives from frame
		// to frame and a steady scene stops allocating after the first one.
		std::vector<engine::scene::DrawInstance> Instances;
	};

	// Every surface camera in the world, as views the renderer takes.
	//
	// **All of them since v0.8, and it used to be the first by entity id.** The
	// pipeline rendered one offscreen view, so a world with four mirrored walls
	// got one working mirror and three panes projecting that one camera's image
	// across themselves — which looked like a bug in the mirror rather than the
	// limit of the pipeline it was. `render::SurfaceView::Index` and
	// `scene::MAX_SURFACES` are what replaced it.
	//
	// **Ordered by entity id, which is creation order**, so a world loaded the
	// same way twice produces the same list. An archetype walk would return
	// whichever row happened to be first, and that moves when anything changes a
	// component set.
	//
	// **Reads what a camera *is*, never where it should be.** Aiming is
	// `scene::AimSurfaceCameras` in `PreRender`, and this runs after it — so a
	// view carries the frame that system computed. Two things deriving a
	// reflection would be two answers to one question, and the one on screen
	// would be whichever ran last.
	//
	// @param store The world to search.
	// @param views Cleared and filled. Kept capacity, so a steady scene stops
	//              allocating after the first frame.
	// @return How many views were written. Zero is the ordinary case in a scene
	//         with no mirror in it, and is not a failure.
	size_t CollectSurfaceViews(engine::ecs::Store &store, std::vector<engine::render::SurfaceView> &views);

	// Turns a world's particle pool into the batches the renderer draws.
	//
	// **One batch per emitter that has live particles, and a span rather than a
	// copy.** The pool's blocks are contiguous per emitter with the live ones a
	// prefix — `effects::ParticleSystem` — so a batch is that prefix pointed at,
	// and half a million particles reach the renderer without being moved.
	//
	// **Here rather than in `effects`, for `CollectSurfaceViews`'s reason.**
	// `render::ParticleBatch` is a `client`-tier type and `effects` is `shared`;
	// a module that named it would be a shared module naming a presentation type.
	// So the pool knows nothing about batches and this is where the two meet.
	//
	// The emitter's shared half — texture, blend mode, flipbook layout, Z offset —
	// is read off `ParticleEmitter` here, once per emitter rather than once per
	// particle, which is the whole reason `ParticleInstance` is twenty-eight
	// bytes.
	//
	// @param store   The world.
	// @param batches Cleared and filled. Valid until the world's pool is stepped
	//                again, which is why the caller submits within the frame.
	// @return How many batches have something to draw.
	size_t
	CollectParticleBatches(engine::ecs::Store &store, std::vector<engine::render::ParticleBatch> &batches);

	// Turns a world's `scene::Light` rows into the lights the renderer takes.
	//
	// **Resolves where each one shines from, which is its parent's business.**
	// A `Light` carries no position — `scene/Components.hpp` refuses it one, for
	// `Sound`'s reason — so this walks one step to the parent and takes its
	// `Attachment`'s world frame or its `Transform`. A light parented to neither
	// is skipped rather than placed at the origin, which would put a lamp in the
	// middle of every world that had one lying loose in `ReplicatedStorage`.
	//
	// **Capped at `render::MAX_SCENE_LIGHTS`, nearest to the eye first.** The
	// renderer drops anything past the cap and has no idea which lamp matters;
	// choosing is the caller's, and distance is the only ordering that is right
	// more often than it is wrong. A scene that needs a better rule — a boss's
	// aura outranking a corridor sconce — wants an authored priority, which is
	// the same shape `replication::DistancePriority` already has and is not in
	// v0.10.
	//
	// @param store  The world.
	// @param eye    Where the view is, for the ordering.
	// @param lights Cleared and filled. Keeps its capacity between frames.
	// @return How many lights were written.
	size_t CollectLights(
		engine::ecs::Store &store,
		const engine::core::Vector3 &eye,
		std::vector<engine::render::SceneLight> &lights
	);

	// Builds the scene by running a Luau file instead of a C++ loop.
	//
	// The entities, the components and the systems that move them all come from
	// `engine::examples::LoadScene`, which every program shares. What this adds
	// is the client's half and only that: a camera to look through and a draw
	// list to fill. A server calls the same loader and adds neither.
	//
	// @param store     The world to build into.
	// @param scheduler The systems to install.
	// @param path      The `.luau` file to run.
	// @param reserve   How much draw-list capacity to reserve up front.
	// @return `false` when the script could not be read, compiled or run.
	bool BuildScriptedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve
	);

	// The client's half of a world, installed onto one somebody else built.
	//
	// **`BuildScriptedWorld` is the demo's entry point and this is the general
	// one.** A studio opens a world out of a game file rather than out of a
	// `.luau`, and a world with no draw list is a world that renders as an
	// empty frame — which reads as a broken renderer rather than as a missing
	// system, and cost an afternoon to find once.
	//
	// Installs a `DrawList`, the previous-transform capture that rendering
	// interpolates from, and the collector that fills the list. Does **not**
	// install `move-camera`: a world being edited is looked at through the
	// editor's camera, and a second thing writing the same `Transform` is the
	// bug `BuildScriptedWorld`'s comment describes.
	//
	// @param store     The world.
	// @param scheduler The systems to install into.
	// @param reserve   How much draw-list capacity to reserve up front.
	void
	InstallPresentation(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler, uint32_t reserve = 0);

	// Gives a world an orbiting camera, when it has none of its own.
	//
	// **A scene that placed its own camera keeps it**, which is the rule
	// `BuildScriptedWorld` already holds and the reason it is a rule: running
	// the placeholder orbit beside a script that aimed a camera is two things
	// writing one `Transform`, the second winning silently every tick. That is
	// not hypothetical — it made `Mirrors-1-world.luau` compute its reflection
	// for a position the viewer was no longer at, and the mirror looked broken.
	//
	// A game file's world usually has no camera, because a camera is something
	// a script makes and the studio does not author one. So single-player needs
	// this and the editor does not: the editor looks through its own camera,
	// which is not an entity in any world.
	//
	// @param store     The world.
	// @param scheduler The systems to install into.
	// @return `true` when a camera was installed, `false` when the world
	//         already had one.
	bool InstallDefaultCamera(engine::ecs::Store &store, engine::ecs::Scheduler &scheduler);

	// Registers this module's own types under explicit names.
	//
	// **One type, and it had no registration at all until v0.7.** `DrawList` is
	// a resource, a resource is keyed by a component id, and
	// `Store::SetResource` was minting one under the compiler's spelling of the
	// type — which is rule 4's exact failure and sat unnoticed because nothing
	// had ever snapshotted a world that had one. The studio's Stop does.
	//
	// Idempotent, and called by both entry points above. Call it before
	// anything touches a `DrawList`: `Components::Of<T>` caches its answer per
	// type per process, so an explicit registration arriving second aborts
	// rather than leaving two names for one thing.
	void RegisterClientComponents();
}
