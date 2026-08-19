#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableMesh.hpp>

#include <client/EditableMeshes.hpp>

namespace client {

	engine::assets::MeshData BuildMeshData(const engine::scene::EditableMesh &mesh) {
		// **Built fresh from the raw arrays rather than kept incrementally**,
		// because `AddVertex`/`AddTriangle` are script-rate calls - a
		// handful a frame at most, on a mesh a player is actively editing -
		// and `render::MeshTable::Add` already re-uploads its whole shared
		// buffer on any change. Converting incrementally would be machinery
		// built for a cost this path does not have.
		engine::assets::MeshData built;
		built.Vertices.reserve(mesh.Positions.size());
		for (size_t index = 0; index < mesh.Positions.size(); index++) {
			engine::assets::MeshVertex vertex{};
			vertex.Position[0] = mesh.Positions[index].X;
			vertex.Position[1] = mesh.Positions[index].Y;
			vertex.Position[2] = mesh.Positions[index].Z;
			vertex.Normal[0] = mesh.Normals[index].X;
			vertex.Normal[1] = mesh.Normals[index].Y;
			vertex.Normal[2] = mesh.Normals[index].Z;
			vertex.TexCoord[0] = mesh.UVs[index].X;
			vertex.TexCoord[1] = mesh.UVs[index].Y;
			built.Vertices.push_back(vertex);
		}
		built.Indices = mesh.Indices;

		// **One submesh, carrying the average of the mesh's own per-vertex
		// colours as its flat one.** `assets::Submesh` has no per-vertex
		// colour of its own - `render::MeshTable` was built for imported
		// content, where a colour is a material's and not a vertex's - so a
		// per-vertex paint job is honoured as far as the format this
		// converts to can carry it. The renderer's own instance tint still
		// applies on top, exactly as it does for any other mesh.
		if (!mesh.Colours.empty()) {
			engine::assets::Submesh submesh;
			submesh.FirstIndex = 0;
			submesh.IndexCount = static_cast<uint32_t>(built.Indices.size());
			float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
			for (size_t index = 0; index < mesh.Colours.size(); index++) {
				r += mesh.Colours[index].R;
				g += mesh.Colours[index].G;
				b += mesh.Colours[index].B;
				a += mesh.Alphas[index];
			}
			const float count = static_cast<float>(mesh.Colours.size());
			submesh.BaseColour[0] = r / count;
			submesh.BaseColour[1] = g / count;
			submesh.BaseColour[2] = b / count;
			submesh.BaseColour[3] = 1.0f - (a / count);
			built.Submeshes.push_back(submesh);
		}

		built.ComputeBounds();
		return built;
	}

	size_t EditableMeshUploader::Refresh(engine::ecs::Store &store, engine::render::Renderer &renderer) {
		size_t uploaded = 0;

		store.Each<const engine::scene::EditableMesh>([&](engine::ecs::Entity entity,
														  const engine::scene::EditableMesh &mesh) {
			const auto found = Uploaded.find(entity.Id);
			if (found != Uploaded.end() && found->second == mesh.Revision) {
				// The steady state: an integer compare, for
				// `ShaderLibrary::Refresh`'s exact reason.
				return;
			}

			const engine::assets::MeshData built = BuildMeshData(mesh);
			if (!built.IsValid()) {
				// An author mid-edit - vertices added, no triangle yet -
				// is the ordinary state right after `Instance.
				// new("EditableMesh")` and must not be reported as a
				// failure.
				return;
			}

			const engine::core::Name name = engine::scene::EditableMeshContentName(store, entity);
			if (renderer.AddMesh(name, built)) {
				Uploaded[entity.Id] = mesh.Revision;
				uploaded++;
			}
		});

		return uploaded;
	}
}
