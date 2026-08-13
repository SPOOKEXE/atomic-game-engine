#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>

#include <algorithm>
#include <utility>
#include <vector>

namespace engine::scene {

	MaterialMaps MaterialCatalogue::Find(const core::Name &material) const {
		if (!material.IsValid()) {
			return {};
		}
		const auto found = ColourMaps.find(material.Id());
		return found == ColourMaps.end() ? MaterialMaps{} : found->second;
	}

	MaterialCatalogue &MaterialsOf(ecs::Store &store) {
		if (!store.HasResource<MaterialCatalogue>()) {
			store.SetResource(MaterialCatalogue{});
		}
		return *store.ResourceMutable<MaterialCatalogue>();
	}

	bool RecordMaterial(ecs::Store &store, const core::Name &material, const MaterialMaps &maps) {
		if (!material.IsValid()) {
			return false;
		}

		// An untextured material is stored rather than rejected, for
		// `RecordTexture`'s reason: it reads back identically to "not known",
		// both mean the same thing to a caller — there is nothing here to sample
		// — and a separate "known to have no texture" state would be a
		// distinction nothing can act on.
		MaterialsOf(store).ColourMaps[material.Id()] = maps;
		return true;
	}

	MaterialMaps ColourMapOf(const ecs::Store &store, const core::Name &material) {
		// **Never creates the resource**, unlike `MaterialsOf`. This is what the
		// resolve pass calls, and a read that mutated the world would put a
		// structural change inside iteration.
		const MaterialCatalogue *catalogue = store.Resource<MaterialCatalogue>();
		return catalogue == nullptr ? MaterialMaps{} : catalogue->Find(material);
	}

	size_t ResolveMaterials(ecs::Store &store) {
		size_t resolved = 0;

		// The parents written this pass, so the next one can tell what stopped
		// having a material. Gathered while writing rather than in a second
		// walk — the entity is already in hand.
		std::vector<ecs::Entity> written;

		// **`Each` and not `EachBatchParallel`, and the difference is the parent
		// lookup** — `ResolveAttachments` states the argument and this pass has
		// exactly the same shape. A batched walk is handed columns and no entity,
		// so there is no handle to ask `ParentOf` about, and the parent's
		// `SurfaceAppearance` lives in a different archetype from the material's
		// row either way.
		//
		// A world with tens of thousands of material instances would want the
		// resolved name denormalised by whatever writes it; a world with one per
		// distinct-looking part does not. Stated rather than measured, which is
		// the honest label.
		store.Each<MaterialRef>([&store, &resolved, &written](ecs::Entity entity, MaterialRef &material) {
			const ecs::Entity parent = store.ParentOf(entity);
			if (parent == ecs::NULL_ENTITY) {
				return;
			}

			// **A `SurfaceAppearance` is the test rather than a class check**,
			// which is `ParentPlacement`'s rule one component over: anything that
			// can be drawn carries one, and asking `IsA("BasePart")` would refuse
			// something that legitimately has the field.
			SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(parent);
			if (appearance == nullptr) {
				return;
			}

			// **Written even when it resolves to nothing**, so a material set
			// back to `None` clears the parent rather than leaving whatever it
			// last pointed at. That is the case `MaterialRef::Asset` calls the
			// honest default, and a pass that skipped it would make "None" mean
			// "keep the previous one".
			// **All five together**, because a part half-updated from a material
			// is worse than one not updated at all: it would draw this
			// material's colour with the last one's normals.
			const MaterialMaps maps = ColourMapOf(store, material.Asset);
			appearance->ColourMap = maps.Colour;
			appearance->NormalMap = maps.Normal;
			appearance->RoughnessMap = maps.Roughness;
			appearance->OcclusionMap = maps.Occlusion;
			appearance->HeightMap = maps.Height;
			appearance->EmissiveMap = maps.Emissive;

			// **From the material instance and not from the catalogue**, which
			// is the one field here that does not come out of a published
			// `.amat`. A shader is selected on the `Material` in the world —
			// `MaterialRef::Shader` — because it may name a `ShaderScript` that
			// only that world contains, and a catalogue keyed by asset name has
			// nowhere to put an answer that differs per world.
			appearance->Shader = material.Shader;
			written.push_back(parent);
			resolved++;
		});

		std::sort(written.begin(), written.end(), [](ecs::Entity left, ecs::Entity right) {
			return left.Id < right.Id;
		});

		// **A world that has never had a material does not acquire a catalogue
		// here.** `ColourMapOf` is deliberately the non-creating reader so this
		// pass leaves an untouched world untouched, and creating the resource to
		// store an empty list would undo that for every world in the universe.
		MaterialCatalogue *catalogue = store.ResourceMutable<MaterialCatalogue>();
		if (catalogue == nullptr) {
			if (written.empty()) {
				return resolved;
			}
			catalogue = &MaterialsOf(store);
		}

		// Clear the parents that had a material last pass and do not now.
		//
		// **Written back even though it resolves to nothing**, which is the same
		// rule as the `None` case above one level out: the field is derived, so
		// the pass owns it, and leaving the last value would make "no material"
		// mean "keep whatever the deleted one said".
		for (const ecs::Entity previous : catalogue->Resolved) {
			const bool still = std::binary_search(
				written.begin(), written.end(), previous, [](ecs::Entity left, ecs::Entity right) {
					return left.Id < right.Id;
				}
			);
			if (still) {
				continue;
			}

			// Null for an entity destroyed since the last pass, for one whose
			// index has been recycled into a different entity, and for a parent
			// that no longer draws. All three are misses rather than special
			// cases.
			if (SurfaceAppearance *appearance = store.GetMutable<SurfaceAppearance>(previous)) {
				// Every map, for the reason the write above gives: clearing the
				// colour and leaving the normals would draw the part untextured
				// but still bumpy.
				appearance->ColourMap = core::Name{};
				appearance->NormalMap = core::Name{};
				appearance->RoughnessMap = core::Name{};
				appearance->OcclusionMap = core::Name{};
				appearance->HeightMap = core::Name{};
				appearance->EmissiveMap = core::Name{};

				// **And the shader with them**, for the same reason: a part
				// whose material was deleted would otherwise go on being drawn
				// by that material's shader, which is the one thing about it
				// still visible after the textures had gone.
				appearance->Shader = core::Name{};
			}
		}

		catalogue->Resolved = std::move(written);
		return resolved;
	}

	ecs::ClassId MaterialClass() {
		// Through `PartClass` for `AttachmentClass`'s reason: one registration of
		// the whole tree, whichever class a caller asks for first.
		EnsureClassTree();
		return ecs::Classes::Find(core::Name("Material"));
	}
}
