#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/Part.hpp>

namespace engine::scene {

	core::Name MaterialCatalogue::Find(const core::Name &material) const {
		if (!material.IsValid()) {
			return {};
		}
		const auto found = ColourMaps.find(material.Id());
		return found == ColourMaps.end() ? core::Name{} : found->second;
	}

	MaterialCatalogue &MaterialsOf(ecs::Store &store) {
		if (!store.HasResource<MaterialCatalogue>()) {
			store.SetResource(MaterialCatalogue{});
		}
		return *store.ResourceMutable<MaterialCatalogue>();
	}

	bool RecordMaterial(ecs::Store &store, const core::Name &material, const core::Name &colour) {
		if (!material.IsValid()) {
			return false;
		}

		// An untextured material is stored rather than rejected, for
		// `RecordTexture`'s reason: it reads back identically to "not known",
		// both mean the same thing to a caller — there is nothing here to sample
		// — and a separate "known to have no texture" state would be a
		// distinction nothing can act on.
		MaterialsOf(store).ColourMaps[material.Id()] = colour;
		return true;
	}

	core::Name ColourMapOf(const ecs::Store &store, const core::Name &material) {
		// **Never creates the resource**, unlike `MaterialsOf`. This is what the
		// resolve pass calls, and a read that mutated the world would put a
		// structural change inside iteration.
		const MaterialCatalogue *catalogue = store.Resource<MaterialCatalogue>();
		return catalogue == nullptr ? core::Name{} : catalogue->Find(material);
	}

	size_t ResolveMaterials(ecs::Store &store) {
		size_t resolved = 0;

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
		store.Each<MaterialRef>([&store, &resolved](ecs::Entity entity, MaterialRef &material) {
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
			appearance->ColourMap = ColourMapOf(store, material.Asset);
			resolved++;
		});

		return resolved;
	}

	ecs::ClassId MaterialClass() {
		// Through `PartClass` for `AttachmentClass`'s reason: one registration of
		// the whole tree, whichever class a caller asks for first.
		PartClass();
		return ecs::Classes::Find(core::Name("Material"));
	}
}
