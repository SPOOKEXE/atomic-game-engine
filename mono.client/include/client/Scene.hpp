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
// world-scoped data as resources. That is not tidiness - a scene class with the
// draw list and the clock as members puts the state the renderer reads outside
// the world, where the affinity check does not cover it, the profiler does not
// see it, and a second world cannot have its own.

#include <engine/core/types/Vector3.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/examples/Scene.hpp>
#include <engine/graph/PipelineDocument.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/DrawInstance.hpp>
#include <engine/world/Universe.hpp>

#include <cstdint>
#include <span>
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
	// across themselves - which looked like a bug in the mirror rather than the
	// limit of the pipeline it was. `render::SurfaceView::Index` and
	// `scene::MAX_SURFACES` are what replaced it.
	//
	// **Ordered by entity id, which is creation order**, so a world loaded the
	// same way twice produces the same list. An archetype walk would return
	// whichever row happened to be first, and that moves when anything changes a
	// component set.
	//
	// **Reads what a camera *is*, never where it should be.** Aiming is
	// `scene::AimSurfaceCameras` in `PreRender`, and this runs after it - so a
	// view carries the frame that system computed. Two things deriving a
	// reflection would be two answers to one question, and the one on screen
	// would be whichever ran last.
	//
	// @param store The world to search.
	// @param views Cleared and filled. Kept capacity, so a steady scene stops
	//              allocating after the first frame.
	// @return How many views were written. Zero is the ordinary case in a scene
	//         with no mirror in it, and is not a failure.
	// @param portals Optional. Any same-world portal in it is skipped, because
	//                the recursive pass draws that pane and a surface camera
	//                aimed at the same slot would render a texture nothing
	//                samples. Pass what `CollectPortalViews` filled.
	size_t CollectSurfaceViews(
		engine::ecs::Store &store,
		std::vector<engine::render::SurfaceView> &views,
		std::span<const engine::render::PortalView> portals = {}
	);

	// Every same-world hole in the world, as the recursive pass takes them.
	//
	// **The other half of the split `render::PortalView` states**: a portal with
	// a `DestinationWorld` is a window onto a second simulation and keeps its
	// surface camera, and one without is a hole in this space and is drawn by
	// recursion instead. `scene::PortalSeam::Crosses` is the test, so the two
	// halves cannot disagree about which a pane is - it is the same field
	// `AppendPortalClones` and `CrossPortals` already branch on.
	//
	// **Built from `GatherPortalSeams` and `SeamMapping`, not from a second
	// derivation of them.** The rectangle a hole is, and the map through it, are
	// what traversal already computes every tick; a picture that measured them
	// its own way would be a picture that disagrees with where a body comes out.
	//
	// **Both maps, front and back.** Which one applies is a question about the
	// camera, and the camera moves with the recursion - see
	// `render::PortalView::Front`.
	//
	// @param store   The world to search.
	// @param portals Cleared and filled, keeping capacity.
	// @return How many holes were written. Zero for a world with no portal in it
	//         and for one whose portals all name other worlds.
	// @since v0.15
	size_t CollectPortalViews(engine::ecs::Store &store, std::vector<engine::render::PortalView> &portals);

	// Points a cross-world portal's surface at the world it names.
	//
	// **The half of a portal a store cannot do for itself.** `AimSurfaceCameras`
	// places the camera and fits its frustum, and both are arithmetic inside one
	// world; what is *drawn* through that frustum is a draw list, and the draw
	// list of another world is on the far side of a boundary rule 3 keeps shut.
	// So the host - which holds the universe and is already outside every store
	// when it calls the renderer - is the only thing that can join the two.
	//
	// **Fills a list of its own, and that separation is the whole contract.**
	// The renderer uploads one instance buffer per frame, so the far world's
	// rows do end up on the end of this world's - but the *appending* is the
	// renderer's to do, after it has culled, ordered and partitioned this
	// world's list. Handing the two joined would put the far world into this
	// world's frustum cull, its scene plan and its main pass: the two rooms
	// drawn on top of each other, which is exactly what happened until v0.14.
	//
	// So `foreign` is indexed from zero and `render::SurfaceView::InstanceFirst`
	// is an offset into it, not into the world's own draw list. A frame with no
	// cross-world portal in it appends nothing and leaves both alone.
	//
	// **Reads the far world's published `DrawList` and does not build one.** A
	// world that is not ticking has whatever its last tick published, which is
	// right - a suspended destination should look like the moment it stopped
	// rather than like nothing. `ImmersivePortals.luau` holds both ends awake so
	// that case does not arise, and says why.
	//
	// **A name matching no world is left alone**, so the pane keeps showing this
	// world. That is the same fallback an unlinked portal already has, and fails
	// the same visible way rather than as a blank pane.
	//
	// ## A hole has two mouths, and both of them are this pass's
	//
	// A body standing in a cross-world pane is in two rooms, exactly as it is
	// for a same-world one - and neither store can say so, because each holds
	// one of the two. So both halves are assembled here, and they go to
	// different places:
	//
	// - a body in **this** world, standing in this world's pane, is cloned onto
	//   the end of `foreign` - into the picture the glass shows;
	// - a body in the **far** world, standing in the far world's pane back to
	//   here, is cloned onto the end of `drawn` - into this room, in front of
	//   this world's pane, where its near half actually stands.
	//
	// **Only the first of those existed until v0.15, and one direction is what
	// that looks like.** Walk into the hole from world A and your far half
	// appears in B's picture; stand in B while somebody walks in from A and
	// nothing comes out of the block. The second half is not the first one
	// repeated from the other side - nobody draws world B's frame while world A
	// is on screen - so it has to be gathered while the far store is open and
	// appended here.
	//
	// **The far pane is found by name, not by surface slot.** A slot numbers a
	// camera within one store; the near pane's index says nothing about which of
	// the far world's cameras leads back, so the pane that leads home is the one
	// whose `Portal::DestinationWorld` names this world.
	//
	// **Driven by this world's panes**, so a pair with a mouth at only one end
	// brings nothing back. That is the same rule the picture already follows:
	// this pass knows which worlds *this* one looks into and does not search the
	// universe for worlds looking at it.
	//
	// @param universe The worlds, entered one at a time and never nested.
	// @param world    The world being drawn.
	// @param drawn    This world's own rows, appended to - never cleared. The
	//                 far side of anybody standing in a far pane that leads
	//                 here ends up on the end of it.
	// @param foreign  Cleared, then filled with every other world's rows this
	//                 frame needs. Hand it to `render::Renderer::Render` as its
	//                 `foreign` argument, beside - never joined to - this
	//                 world's own draw list.
	// @param views    The views `CollectSurfaceViews` filled, updated in place.
	// @return How many surfaces were pointed at another world. The clones
	//         appended to `drawn` are not counted: the question is how many
	//         panes were given a picture.
	// @since v0.14
	size_t AttachForeignSurfaces(
		engine::world::Universe &universe,
		engine::world::WorldId world,
		std::vector<engine::scene::DrawInstance> &drawn,
		std::vector<engine::scene::DrawInstance> &foreign,
		std::vector<engine::render::SurfaceView> &views
	);

	// Turns a world's particle pool into the batches the renderer draws.
	//
	// **One batch per emitter that has live particles, and a span rather than a
	// copy.** The pool's blocks are contiguous per emitter with the live ones a
	// prefix - `effects::ParticleSystem` - so a batch is that prefix pointed at,
	// and half a million particles reach the renderer without being moved.
	//
	// **Here rather than in `effects`, for `CollectSurfaceViews`'s reason.**
	// `render::ParticleBatch` is a `client`-tier type and `effects` is `shared`;
	// a module that named it would be a shared module naming a presentation type.
	// So the pool knows nothing about batches and this is where the two meet.
	//
	// The emitter's shared half - texture, blend mode, flipbook layout, Z offset -
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
	// A `Light` carries no position - `scene/Components.hpp` refuses it one, for
	// `Sound`'s reason - so this walks one step to the parent and takes its
	// `Attachment`'s world frame or its `Transform`. A light parented to neither
	// is skipped rather than placed at the origin, which would put a lamp in the
	// middle of every world that had one lying loose in `ReplicatedStorage`.
	//
	// **Capped at `render::MAX_SCENE_LIGHTS`, nearest to the eye first.** The
	// renderer drops anything past the cap and has no idea which lamp matters;
	// choosing is the caller's, and distance is the only ordering that is right
	// more often than it is wrong. A scene that needs a better rule - a boss's
	// aura outranking a corridor sconce - wants an authored priority, which is
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
	// **The runtime comes back**, because the client has to be able to reach the
	// VM of the world it is drawing: `Runtime::DeliverGuiEvents` is how a
	// `TextButton`'s `Activated` gets from `gui::Router` to a script, and until
	// v0.15 this loader kept the only reference. A shipped client running a
	// `--script` scene therefore routed its interface input, produced the
	// events, and had nowhere to deliver them - every button in every scripted
	// scene was silent in the one program a game ships.
	//
	// @param store     The world to build into.
	// @param scheduler The systems to install.
	// @param path      The `.luau` file to run.
	// @param reserve   How much draw-list capacity to reserve up front.
	// @param runtime   Set to the VM that ran the scene, when not null.
	// @return `false` when the script could not be read, compiled or run.
	bool BuildScriptedWorld(
		engine::ecs::Store &store,
		engine::ecs::Scheduler &scheduler,
		const std::string &path,
		uint32_t reserve,
		std::shared_ptr<engine::script::Runtime> *runtime = nullptr
	);

	// The client's half of a world, installed onto one somebody else built.
	//
	// **`BuildScriptedWorld` is the demo's entry point and this is the general
	// one.** A studio opens a world out of a game file rather than out of a
	// `.luau`, and a world with no draw list is a world that renders as an
	// empty frame - which reads as a broken renderer rather than as a missing
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
	// not hypothetical - it made `Mirrors-1-world.luau` compute its reflection
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

	// Installs one universe rendering profile under a key qualified by world.
	//
	// The renderer owns compiled device plans and knows nothing about stores.
	// This client-tier join reads the universe `PipelineSet`, removes a stale key
	// from an earlier install, and hands one complete graph across. Two worlds may
	// select the same profile without colliding because the installed key
	// includes `world`.
	//
	// The world's selected profile is tried first. Otherwise `Default PBR` and
	// then each remaining profile are tried, which also recovers from an invalid
	// authored graph. Profiles that this world does not select stay as documents
	// and consume no runtime renderer resources.
	// An invalid return selects the renderer's standard frame.
	//
	// @param profiles The universe's authored documents.
	// @param renderer The runtime cache to update.
	// @param world    The stable world number used to qualify keys.
	// @param selected The profile selected in this world's settings.
	// @return The key this world's primary view should select.
	engine::core::Name InstallRenderingProfiles(
		const engine::graph::PipelineSet &profiles,
		engine::render::Renderer &renderer,
		uint64_t world,
		engine::core::Name selected
	);

	// Registers this module's own types under explicit names.
	//
	// **One type, and it had no registration at all until v0.7.** `DrawList` is
	// a resource, a resource is keyed by a component id, and
	// `Store::SetResource` was minting one under the compiler's spelling of the
	// type - which is rule 4's exact failure and sat unnoticed because nothing
	// had ever snapshotted a world that had one. The studio's Stop does.
	//
	// Idempotent, and called by both entry points above. Call it before
	// anything touches a `DrawList`: `Components::Of<T>` caches its answer per
	// type per process, so an explicit registration arriving second aborts
	// rather than leaving two names for one thing.
	void RegisterClientComponents();
}
