#include <engine/ecs/Store.hpp>
#include <engine/scene/PublishedCatalogue.hpp>

namespace engine::scene {

	PublishedCatalogue &PublishedOf(ecs::Store &store) {
		if (!store.HasResource<PublishedCatalogue>()) {
			store.SetResource(PublishedCatalogue{});
		}
		return *store.ResourceMutable<PublishedCatalogue>();
	}

	size_t RecordPublishedMeshes(ecs::Store &store, const std::vector<core::Name> &meshes) {
		PublishedCatalogue &catalogue = PublishedOf(store);

		catalogue.Meshes.clear();
		catalogue.Meshes.reserve(meshes.size());
		for (const core::Name &mesh : meshes) {
			if (mesh.IsValid()) {
				catalogue.Meshes.push_back(mesh);
			}
		}
		return catalogue.Meshes.size();
	}

	size_t PublishedMeshes(const ecs::Store &store, std::vector<core::Name> &out) {
		// **Never creates the resource**, unlike `PublishedOf`. A script binding
		// calls this, and a read that mutated the world to answer would be a
		// structural change from inside a script's own frame.
		const PublishedCatalogue *catalogue = store.Resource<PublishedCatalogue>();
		if (catalogue == nullptr) {
			return 0;
		}

		out.insert(out.end(), catalogue->Meshes.begin(), catalogue->Meshes.end());
		return catalogue->Meshes.size();
	}
}
