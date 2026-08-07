#pragma once

// What a store has published, as opposed to what this world has loaded.
//
// **These are two different questions and only one of them had an answer.**
// `MeshCatalogue` holds what arrived — the meshes something named, fetched and
// registered. Since v0.10 nothing is fetched by kind, so that set is exactly
// what has already been asked for, and a scene reading it can never be the thing
// that asks. `MeshGrid.luau` ran into that head on: it laid out whatever
// `ContentService:GetMeshes()` reported, nothing else named the store's imports,
// so the plate held the six built-ins and a store of a thousand assets was
// invisible to it.
//
// The missing half is the *manifest*. A publisher signed a list of names and
// kinds; a client verifies it before it fetches anything, so by the time content
// is reachable at all this list is known. It was simply never handed to the
// world.
//
// **Naming something out of this list is what fetches it**, and that is the
// whole point rather than a side effect. `client::CollectWantedContent` walks the
// world for names, so a scene that reads this and sets a `MeshId` has *named*
// the mesh, and the next content pump asks for that one asset. The v0.10 rule
// holds unchanged: a world names it or it is not fetched. What changes is that a
// world can now find out what there is to name.
//
// That is different from asking by kind in the way that matters. Asking by kind
// pulled every bundle in the store through one synchronous `Pump` — 6.9 GB on
// this repository's own store, `client/ContentDemand.hpp` has the numbers. This
// hands over a few hundred *strings*, and a scene decides how many of them to
// use. `MeshGrid.luau` uses twelve.
//
// ## Why names and not entries
//
// `assets::AssetKind` lives in `engine/assets` and this module depends on `core`,
// `ecs` and `spatial` — see `scene/AGENTS.md`, that list is not growing. So the
// filtering by kind happens in whoever read the manifest, and what crosses into
// the world is the same thing that crosses everywhere else in this engine: a
// name. `MeshCatalogue` makes the identical trade for the identical reason,
// taking a triangle count rather than an `assets::MeshData`.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>

#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// The names a store has published, by kind, as far as this world was told.
	//
	// **Derived rather than authored, so it is not saved.** Its registration
	// writes nothing and reads back empty, exactly as `MeshCatalogue`'s does: the
	// contents come from a manifest verified this run, and a save file carrying
	// last run's list would name assets that may no longer be published.
	//
	// @since v0.10
	struct PublishedCatalogue {
		// Published meshes, in the order they were recorded.
		//
		// **Meshes only, and the gap is deliberate rather than an oversight.**
		// The one caller that needs this is a scene choosing geometry, and a
		// vector per kind added before anything reads it would be four lists to
		// keep in step for the sake of symmetry. Textures are the obvious next
		// one and belong here when something wants them.
		//
		// **A vector rather than a set**, because a manifest already holds each
		// name once and the reader iterates far more often than it inserts.
		// `RecordPublishedMeshes` replaces the whole list, so a republish cannot
		// leave a stale name behind.
		std::vector<core::Name> Meshes;
	};

	// The world's published catalogue, creating an empty one if it has none.
	//
	// **`RegisterSceneComponents` must have run first**, as it must before any
	// resource here is set — `MeshesOf` carries the whole argument, including why
	// getting it wrong passes most runs.
	//
	// @param store The world.
	// @return The catalogue.
	PublishedCatalogue &PublishedOf(ecs::Store &store);

	// Replaces the list of published meshes.
	//
	// **Replaces rather than appends**, because a manifest is a whole answer. A
	// second publish that dropped an asset would otherwise leave its name in a
	// list a scene is choosing from, and the failure is a cell that draws the
	// fallback cube with nothing to say why.
	//
	// @param store  The world.
	// @param meshes The published mesh names. Invalid ones are dropped rather
	//        than stored — a name a scene cannot use is a row it would try to
	//        fetch and never resolve.
	// @return How many were kept.
	size_t RecordPublishedMeshes(ecs::Store &store, const std::vector<core::Name> &meshes);

	// What the store published, or nothing when this world has not been told.
	//
	// **The `const` reader, so a script binding can use it**: it never creates
	// the resource. A world with none answers empty rather than acquiring one
	// from inside a read — `TrianglesOf`'s reason, and the same trap.
	//
	// @param store The world.
	// @param out   Appended to, so a caller can gather several worlds.
	// @return How many were appended.
	size_t PublishedMeshes(const ecs::Store &store, std::vector<core::Name> &out);
}
