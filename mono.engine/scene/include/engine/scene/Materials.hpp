#pragma once

// What a part is made of, as an asset rather than as a word.
//
// **This replaces `Enum.Material`, and the enum is gone rather than deprecated.**
// It held seventeen names — `Plastic`, `Wood`, `Metal` — checked against a
// registered set, and the check was the only thing it did: nothing in the
// renderer sampled a different texture because a part said `Wood`, because a
// name is not a texture. `AGENTS.md` calls two ways to do one job the most
// expensive debt in a monorepo, and a material enum beside a material asset is
// exactly that, so `Visual::Material` and the `Material` `EnumTable` entry are
// deleted with the thing that replaces them.
//
// ## A `Material` is an instance, not a field
//
// `Instance.new("Material")` under a `BasePart`, which is Roblox's arrangement
// for `SurfaceAppearance` and is what `ROADMAP.md` v0.11's node-based asset
// pipeline needs: a material that will eventually carry several texture
// references has to be a *thing* with children, and a `core::Name` field on
// `BasePart` could never grow one.
//
// **`Attachment`'s shape exactly**, down to the reason: an instance whose only
// job is to say something about its parent, resolved by one flat pass over one
// component type. It is an `Instance` and not a `PVInstance` for the same reason
// an attachment is — it has no place in the world of its own.
//
// ## The resolve pass, and what it writes
//
// `ResolveMaterials` walks every `MaterialRef` row, looks its asset up in the
// world's `MaterialCatalogue`, and writes the resulting texture name into the
// **parent's** `SurfaceAppearance::ColourMap`. That is the field the draw-list
// pass already reads — `client::CollectInstances` is a batched parallel loop
// over a fixed signature, and a child lookup is precisely what that shape cannot
// express, which is the same wall `SurfaceAppearance`'s own comment hits.
//
// **A part with no `Material` child is not visited at all**, so authoring
// `BasePart.ColorMap` directly still works and still means what it did. A part
// with both is authored twice and the material wins, because it is the more
// specific statement.
//
// **A part that stops having a material is cleared, and the pass costs nothing
// extra to do it.** Deleting a `Material` child used to leave the colour map it
// last resolved sitting on the parent for ever, because nothing visits a part
// that no longer has one — `docs/DEFERRED.md` D00032. The fix is not the pass
// over every part that entry rejected, which would be a child scan per drawable
// per tick to correct an editor-time action. It is a **difference between two
// passes**: the resolve records which parents it wrote, and the next one clears
// any parent in that record it did not write again. The work is proportional to
// the number of *materials*, which is what the pass already walks.
//
// **That covers more than deletion, which is why it is this and not a
// destruction hook.** A hook on the row leaving would miss a `Material`
// *reparented* to another part, a `MaterialRef` removed from a still-living
// instance, and a part whose material was moved under something with no
// `SurfaceAppearance` — all of which leave the same stale texture and none of
// which is a destruction.
//
// ## Nothing here reads a material file
//
// `scene` is `shared` and does not link `assets`. The catalogue takes a texture
// name from whoever *did* read the `.amat` — the client's content pump, the same
// caller as `RecordMesh` and `RecordTexture` — which is `TextureCatalogue`'s
// rule and its reason.
//
// @tier L7 · shared

#include <engine/core/Name.hpp>
#include <engine/ecs/Classes.hpp>
#include <engine/ecs/Entity.hpp>

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace engine::ecs {
	class Store;
}

namespace engine::scene {

	// What a world knows about the materials its parts name.
	//
	// **Derived rather than authored, so it is not saved** — `TextureCatalogue`'s
	// rule. Its contents come from whatever registered the materials this run,
	// and a save file carrying last run's texture names would be names that agree
	// with nothing on disk.
	//
	// Every texture one material names.
	//
	// **A struct rather than five parallel maps**, because they are written and
	// read together on every path — recorded from one `.amat`, resolved onto one
	// part, demanded as one set. Five maps keyed the same way would be five
	// places for a material to be half-present.
	//
	// @since v0.11
	// **Every map is initialised where it is declared**, which is what lets a
	// caller name the one it has — `MaterialMaps{.Colour = texture}` is the
	// ordinary case, because a material with a colour and nothing else is the
	// ordinary material. Without these `= {}` the omitted five are still
	// value-initialised and still invalid, and GCC still refuses the designated
	// initialiser under `-Wmissing-field-initializers`, which the `ci` preset
	// makes fatal. `core::Name` already defaults to invalid, so this changes no
	// value — it says so where the compiler can see it.
	struct MaterialMaps {
		// The albedo, and the one a material usually has on its own.
		core::Name Colour = {};

		// Tangent-space normals, as the baker writes them.
		core::Name Normal = {};

		// Per-texel roughness. Absent leaves the material's own scalar.
		core::Name Roughness = {};

		// Baked ambient occlusion, multiplied into the ambient term.
		core::Name Occlusion = {};

		// Height, for whatever the pipeline does with it. Written by the baker
		// and carried here because a material names it; nothing samples it yet.
		core::Name Height = {};

		// What the surface emits on its own, independent of any light.
		core::Name Emissive = {};

		// Whether this names anything at all.
		//
		// @return `true` when any map is set. A material naming none is a real
		//         state — see `assets::MaterialData` — so this says "the
		//         catalogue knows this material" rather than "it is usable".
		bool IsValid() const {
			return Colour.IsValid() || Normal.IsValid() || Roughness.IsValid() || Occlusion.IsValid() ||
				   Height.IsValid();
		}
	};

	// @since v0.10
	struct MaterialCatalogue {
		// The colour map each known material names, keyed by `core::Name::Id`.
		//
		// The id as the key and the `Name` as the value, matching
		// `TextureCatalogue::Flipbooks`: a `Name` is already an integer in this
		// process, so hashing the integer skips the registry lock comparing text
		// would take, and the value has to be a `Name` because it is what gets
		// written onto a component.
		std::unordered_map<uint32_t, MaterialMaps> ColourMaps;

		// The parents `ResolveMaterials` wrote on its last pass, sorted.
		//
		// **This is what lets a part be cleared when its material goes away**
		// without visiting every part in the world: the next pass writes its own
		// list and clears whatever is in this one and not in that one.
		//
		// **Handles rather than indices, and the generation is load-bearing.**
		// An entity destroyed between two passes hands its index straight to the
		// next `Create`, so an index alone would clear the colour map of an
		// unrelated part that happened to be built in its place. `Store::Get`
		// checks the generation and answers null for a handle that has been
		// recycled, which turns that into a miss rather than a wrong write.
		//
		// Sorted so the difference is a binary search per entry rather than a
		// scan, and because two `Material` children on one part would otherwise
		// have to be de-duplicated by the reader.
		std::vector<ecs::Entity> Resolved;

		// The colour map a material names, or an invalid name.
		//
		// **An invalid name means "nothing to sample", and that covers two
		// states on purpose**: a material this world has not been told about, and
		// a material that names no texture. Neither is something to draw, and a
		// consumer that had to tell them apart would be asking a question with no
		// use — the renderer draws `render::DefaultTexture` either way.
		//
		// @param material The material's asset name.
		// @return The texture's name, or an invalid one.
		MaterialMaps Find(const core::Name &material) const;
	};

	// The world's material catalogue, creating an empty one if it has none.
	//
	// **`RegisterSceneComponents` must have run first**, as it must before any
	// resource here is set — `MeshesOf` carries why in full.
	//
	// @param store The world.
	// @return The catalogue.
	MaterialCatalogue &MaterialsOf(ecs::Store &store);

	// Records which textures a material's maps name.
	//
	// **Last writer wins, and re-registering is legal**, because that is what the
	// content path does: a publisher may replace a material under a name it
	// already used, and a catalogue that refused the second would keep answering
	// with the texture that is no longer wanted.
	//
	// @param store    The world.
	// @param material The material's asset name.
	// @param maps     The textures its maps name. Every one may be invalid,
	//                 which is a material somebody has not textured yet.
	// @return `false` for an invalid material name.
	bool RecordMaterial(ecs::Store &store, const core::Name &material, const MaterialMaps &maps);

	// Which texture a material resolves to in this world.
	//
	// The `const` reader, so a resolve pass can use it: it never creates the
	// resource, and a world that has none answers an invalid name rather than
	// acquiring an empty catalogue from inside a read.
	//
	// @param store    The world.
	// @param material The material's asset name.
	// @return The texture's name, or an invalid one.
	MaterialMaps ColourMapOf(const ecs::Store &store, const core::Name &material);

	// Writes every material instance's texture onto the part it hangs off.
	//
	// **`PreSimulation`, ahead of anything that reads a `SurfaceAppearance`**,
	// which is where `ResolveAttachments` runs and for the same reason: a
	// consumer reading the derived field on the tick it was authored would
	// otherwise draw last tick's texture for one frame.
	//
	// @param store The world.
	// @return How many material instances resolved onto a parent.
	size_t ResolveMaterials(ecs::Store &store);

	// The `Material` class id, registering the tree if nobody has yet.
	//
	// @return The class id.
	ecs::ClassId MaterialClass();
}
