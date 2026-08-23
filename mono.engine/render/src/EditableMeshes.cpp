#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableMesh.hpp>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::render {

	engine::assets::MeshData BuildMeshData(const engine::scene::EditableMesh &mesh) {
		// **Built once per semantic revision rather than maintained beside the
		// scene arrays.** `EditableMeshUploader` records both drawable and
		// incomplete revisions, and bulk transactions advance one revision for a
		// complete terrain-sized result. A second packed copy here would be shared
		// storage for data the ECS already owns.
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

		// **One submesh run per distinct triangle colour, rather than one run
		// carrying the average of the whole mesh.**
		//
		// `assets::MeshVertex` has no colour field - `render::MeshTable` was
		// built for imported content, where a colour belongs to a material and
		// not to a vertex - so a per-vertex paint job cannot survive as one. It
		// does not follow that it has to collapse to a single average: an
		// `assets::Submesh` carries its own `BaseColour`, `MeshTable::Add` turns
		// every submesh into its own draw run with its own colour, and a mesh
		// may have as many runs as it likes.
		//
		// **Averaging the whole mesh made every script-built mesh one flat
		// colour**, which is the difference between a terrain heightfield that
		// reads as sand, grass, rock and snow and one that reads as a single mud
		// brown - and between a mirror ball with sixteen tinted facets and a
		// uniformly grey sphere. Neither looked like a missing feature; both
		// looked like the colours had simply been authored badly.
		//
		// A triangle's colour is the average of its three corners, which is the
		// same reduction as before at the only scale where it loses nothing: a
		// mesh painted a colour per face has three identical corners per face and
		// comes through exactly. A smooth gradient across a shared vertex still
		// bands, and that is the honest limit of a vertex format with no colour
		// in it.
		//
		// **Quantised to eight bits a channel before grouping**, because that is
		// the precision the colour is eventually shown at, and grouping on raw
		// floats would mint a separate draw run for two colours nothing can tell
		// apart. Runs are emitted in first-appearance order so two conversions of
		// the same mesh produce byte-identical output.
		if (!mesh.Colours.empty() && built.Indices.size() >= 3) {
			struct ColourRun {
				float Red = 0.0f;
				float Green = 0.0f;
				float Blue = 0.0f;
				float Alpha = 0.0f;
				std::vector<uint32_t> Indices;
			};

			std::vector<ColourRun> runs;
			std::unordered_map<uint32_t, size_t> slots;

			const auto channel = [&](size_t vertex, size_t offset) -> float {
				if (vertex >= mesh.Colours.size()) {
					return 1.0f;
				}
				const engine::core::Color3 &colour = mesh.Colours[vertex];
				return offset == 0 ? colour.R : (offset == 1 ? colour.G : colour.B);
			};
			const auto alpha = [&](size_t vertex) -> float {
				return vertex < mesh.Alphas.size() ? mesh.Alphas[vertex] : 0.0f;
			};
			const auto quantise = [](float value) -> uint32_t {
				const float clamped = std::clamp(value, 0.0f, 1.0f);
				return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
			};

			for (size_t first = 0; first + 2 < built.Indices.size(); first += 3) {
				const uint32_t a = built.Indices[first];
				const uint32_t b = built.Indices[first + 1];
				const uint32_t c = built.Indices[first + 2];

				float components[4]{};
				for (size_t offset = 0; offset < 3; offset++) {
					components[offset] =
						(channel(a, offset) + channel(b, offset) + channel(c, offset)) / 3.0f;
				}
				// Stored as opacity, which is what `BaseColour`'s fourth channel
				// means; `EditableMesh::Alphas` holds transparency.
				components[3] = 1.0f - (alpha(a) + alpha(b) + alpha(c)) / 3.0f;

				const uint32_t key = (quantise(components[0]) << 24) | (quantise(components[1]) << 16) |
									 (quantise(components[2]) << 8) | quantise(components[3]);

				const auto found = slots.find(key);
				size_t slot = 0;
				if (found == slots.end()) {
					slot = runs.size();
					slots.emplace(key, slot);
					ColourRun run;
					run.Red = components[0];
					run.Green = components[1];
					run.Blue = components[2];
					run.Alpha = components[3];
					runs.push_back(std::move(run));
				} else {
					slot = found->second;
				}

				runs[slot].Indices.push_back(a);
				runs[slot].Indices.push_back(b);
				runs[slot].Indices.push_back(c);
			}

			// **The index buffer is rewritten so each run is contiguous**, which
			// is what a `Submesh` is: a first index and a count into one list. The
			// vertices are untouched, so no index is remapped and the winding of
			// every triangle survives - only the order the triangles are listed
			// in changes, and nothing downstream depends on that.
			std::vector<uint32_t> ordered;
			ordered.reserve(built.Indices.size());
			for (const ColourRun &run : runs) {
				engine::assets::Submesh submesh;
				submesh.FirstIndex = static_cast<uint32_t>(ordered.size());
				submesh.IndexCount = static_cast<uint32_t>(run.Indices.size());
				submesh.BaseColour[0] = run.Red;
				submesh.BaseColour[1] = run.Green;
				submesh.BaseColour[2] = run.Blue;
				submesh.BaseColour[3] = run.Alpha;
				built.Submeshes.push_back(submesh);

				ordered.insert(ordered.end(), run.Indices.begin(), run.Indices.end());
			}
			built.Indices = std::move(ordered);
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
				// failure. Remembering the revision is important: otherwise an
				// unchanged half-built mesh pays the full conversion every presented
				// frame. The next edit advances the revision and retries it.
				Uploaded[entity.Id] = mesh.Revision;
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
