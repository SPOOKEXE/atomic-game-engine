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

	std::span<const core::Name> MeshCatalogue::Sheets(const core::Name &mesh) const {
		if (!mesh.IsValid()) {
			return {};
		}
		const auto found = Textures.find(mesh.Id());
		return found == Textures.end() ? std::span<const core::Name>{} : found->second;
	}

	MeshCatalogue &MeshesOf(ecs::Store &store) {
		if (!store.HasResource<MeshCatalogue>()) {
			store.SetResource(MeshCatalogue{});
		}
		return *store.ResourceMutable<MeshCatalogue>();
	}

	bool RecordMesh(
		ecs::Store &store, const core::Name &mesh, uint32_t triangles, std::span<const core::Name> sheets
	) {
		if (!mesh.IsValid()) {
			return false;
		}

		// A count of zero is stored rather than rejected. It reads back
		// identically to "not known", which is deliberate: both mean the same
		// thing to a caller — this world cannot tell you — and a separate
		// "known to be empty" state would be a distinction nothing can act on.
		MeshCatalogue &catalogue = MeshesOf(store);
		catalogue.Triangles[mesh.Id()] = triangles;

		// **Replaced rather than merged**, for the reason the header gives: a
		// republished mesh may name different sheets, and a merge would leave a
		// name listed that the geometry no longer wears — which is worse than
		// not knowing, because it is a name somebody would put back.
		//
		// An empty list is *stored* as empty rather than skipped. Every built-in
		// names no sheet at all, and leaving a stale entry there would be the
		// same lie one layer along.
		catalogue.Textures[mesh.Id()].assign(sheets.begin(), sheets.end());
		return true;
	}

	size_t SheetsOf(const ecs::Store &store, const core::Name &mesh, std::vector<core::Name> &out) {
		out.clear();

		// **Never creates the resource**, for `TrianglesOf`'s reason: this is
		// what a script binding calls, and a read that mutated the world would
		// be a structural change inside iteration.
		if (const MeshCatalogue *catalogue = store.Resource<MeshCatalogue>(); catalogue != nullptr) {
			const std::span<const core::Name> sheets = catalogue->Sheets(mesh);
			out.assign(sheets.begin(), sheets.end());
		}
		return out.size();
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
