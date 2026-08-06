#include <engine/ecs/Store.hpp>
#include <engine/scene/MeshCatalogue.hpp>

namespace engine::scene {

	uint32_t MeshCatalogue::Find(const core::Name &mesh) const {
		if (!mesh.IsValid()) {
			return 0;
		}
		const auto found = Triangles.find(mesh.Id());
		return found == Triangles.end() ? 0 : found->second;
	}

	MeshCatalogue &MeshesOf(ecs::Store &store) {
		if (!store.HasResource<MeshCatalogue>()) {
			store.SetResource(MeshCatalogue{});
		}
		return *store.ResourceMutable<MeshCatalogue>();
	}

	bool RecordMesh(ecs::Store &store, const core::Name &mesh, uint32_t triangles) {
		if (!mesh.IsValid()) {
			return false;
		}

		// A count of zero is stored rather than rejected. It reads back
		// identically to "not known", which is deliberate: both mean the same
		// thing to a caller — this world cannot tell you — and a separate
		// "known to be empty" state would be a distinction nothing can act on.
		MeshesOf(store).Triangles[mesh.Id()] = triangles;
		return true;
	}

	uint32_t TrianglesOf(const ecs::Store &store, const core::Name &mesh) {
		// **Never creates the resource**, unlike `MeshesOf`. This is what a
		// property getter calls, and a getter that mutated the world to answer
		// a read would put a structural change inside iteration — and would do
		// it on every part in the scene on the first frame.
		const MeshCatalogue *catalogue = store.Resource<MeshCatalogue>();
		return catalogue == nullptr ? 0 : catalogue->Find(mesh);
	}
}
