#include <engine/assets/Mesh.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/render/EditableMeshes.hpp>
#include <engine/render/Renderer.hpp>
#include <engine/scene/EditableMesh.hpp>

#include <algorithm>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine::render {
	namespace {
		PackedMeshFormat PackedFormatOf(engine::scene::EditablePackingFormat format) {
			switch (format) {
			case engine::scene::EditablePackingFormat::Float32:
				return PackedMeshFormat::Float32;
			case engine::scene::EditablePackingFormat::Float16:
				return PackedMeshFormat::Float16;
			case engine::scene::EditablePackingFormat::Float8E4M3FN:
				return PackedMeshFormat::Float8E4M3FN;
			case engine::scene::EditablePackingFormat::Signed16:
				return PackedMeshFormat::Signed16;
			case engine::scene::EditablePackingFormat::Unsigned16:
				return PackedMeshFormat::Unsigned16;
			case engine::scene::EditablePackingFormat::Signed8:
				return PackedMeshFormat::Signed8;
			case engine::scene::EditablePackingFormat::Unsigned8:
				return PackedMeshFormat::Unsigned8;
			case engine::scene::EditablePackingFormat::Signed4:
				return PackedMeshFormat::Signed4;
			case engine::scene::EditablePackingFormat::Unsigned4:
				return PackedMeshFormat::Unsigned4;
			case engine::scene::EditablePackingFormat::Boolean:
				return PackedMeshFormat::Boolean;
			}
			return PackedMeshFormat::Float32;
		}
	}

	engine::assets::MeshData BuildMeshData(const engine::scene::EditableMesh &mesh) {
		// **Built once per semantic revision rather than maintained beside the
		// scene arrays.** `EditableMeshUploader` records both drawable and
		// incomplete revisions, and bulk transactions advance one revision for a
		// complete terrain-sized result. A second packed copy here would be shared
		// storage for data the ECS already owns.
		engine::assets::MeshData built;
		std::vector<std::byte> packed;
		std::vector<float> decoded;
		const auto quantized = [&](engine::scene::EditablePackingAttribute attribute,
								   std::span<const float> values) {
			if (!mesh.Packing.Enabled() || (mesh.Packing.Attributes & static_cast<uint8_t>(attribute)) == 0)
				return values;
			if (!engine::scene::PackEditableValues(values, mesh.Packing, packed) ||
				!engine::scene::UnpackEditableValues(packed, values.size(), mesh.Packing, decoded))
				return values;
			return std::span<const float>(decoded);
		};
		std::vector<float> positions(mesh.Positions.size() * 3);
		std::vector<float> normals(mesh.Normals.size() * 3);
		std::vector<float> uvs(mesh.UVs.size() * 2);
		for (size_t index = 0; index < mesh.Positions.size(); index++) {
			positions[index * 3] = mesh.Positions[index].X;
			positions[index * 3 + 1] = mesh.Positions[index].Y;
			positions[index * 3 + 2] = mesh.Positions[index].Z;
		}
		for (size_t index = 0; index < mesh.Normals.size(); index++) {
			normals[index * 3] = mesh.Normals[index].X;
			normals[index * 3 + 1] = mesh.Normals[index].Y;
			normals[index * 3 + 2] = mesh.Normals[index].Z;
		}
		for (size_t index = 0; index < mesh.UVs.size(); index++) {
			uvs[index * 2] = mesh.UVs[index].X;
			uvs[index * 2 + 1] = mesh.UVs[index].Y;
		}
		const std::span<const float> packedPositions =
			quantized(engine::scene::EditablePackingAttribute::Position, positions);
		const std::vector<float> positionCopy(packedPositions.begin(), packedPositions.end());
		const std::span<const float> packedNormals =
			quantized(engine::scene::EditablePackingAttribute::Normal, normals);
		const std::vector<float> normalCopy(packedNormals.begin(), packedNormals.end());
		const std::span<const float> packedUVs = quantized(engine::scene::EditablePackingAttribute::UV, uvs);
		const std::vector<float> uvCopy(packedUVs.begin(), packedUVs.end());
		built.Vertices.reserve(mesh.Positions.size());
		for (size_t index = 0; index < mesh.Positions.size(); index++) {
			engine::assets::MeshVertex vertex{};
			vertex.Position[0] = positionCopy[index * 3];
			vertex.Position[1] = positionCopy[index * 3 + 1];
			vertex.Position[2] = positionCopy[index * 3 + 2];
			vertex.Normal[0] = normalCopy[index * 3];
			vertex.Normal[1] = normalCopy[index * 3 + 1];
			vertex.Normal[2] = normalCopy[index * 3 + 2];
			vertex.TexCoord[0] = uvCopy[index * 2];
			vertex.TexCoord[1] = uvCopy[index * 2 + 1];
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

			std::vector<float> colourValues(mesh.Colours.size() * 3);
			for (size_t index = 0; index < mesh.Colours.size(); ++index) {
				colourValues[index * 3] = mesh.Colours[index].R;
				colourValues[index * 3 + 1] = mesh.Colours[index].G;
				colourValues[index * 3 + 2] = mesh.Colours[index].B;
			}
			std::vector<float> alphaValues(mesh.Alphas.begin(), mesh.Alphas.end());
			std::vector<float> colourDecoded;
			std::vector<float> alphaDecoded;
			if (mesh.Packing.Enabled() &&
				(mesh.Packing.Attributes &
				 static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Colour)) != 0 &&
				engine::scene::PackEditableValues(colourValues, mesh.Packing, packed) &&
				engine::scene::UnpackEditableValues(packed, colourValues.size(), mesh.Packing, colourDecoded))
				colourValues = std::move(colourDecoded);
			if (mesh.Packing.Enabled() &&
				(mesh.Packing.Attributes &
				 static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Alpha)) != 0 &&
				engine::scene::PackEditableValues(alphaValues, mesh.Packing, packed) &&
				engine::scene::UnpackEditableValues(packed, alphaValues.size(), mesh.Packing, alphaDecoded))
				alphaValues = std::move(alphaDecoded);

			const auto channel = [&](size_t vertex, size_t offset) -> float {
				if (vertex >= mesh.Colours.size()) {
					return 1.0f;
				}
				return colourValues[vertex * 3 + offset];
			};
			const auto alpha = [&](size_t vertex) -> float {
				return vertex < alphaValues.size() ? alphaValues[vertex] : 0.0f;
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

	PackedMeshData BuildPackedMeshData(const engine::scene::EditableMesh &mesh) {
		constexpr uint8_t allAttributes =
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Position) |
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Normal) |
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::UV) |
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Colour) |
			static_cast<uint8_t>(engine::scene::EditablePackingAttribute::Alpha);
		if (!mesh.Packing.Enabled() || (mesh.Packing.Attributes & ~allAttributes) != 0) return {};

		const engine::assets::MeshData expanded = BuildMeshData(mesh);
		if (!expanded.IsValid()) return {};
		PackedMeshData built;
		built.Indices = expanded.Indices;
		built.Submeshes = expanded.Submeshes;
		built.Minimum = expanded.Minimum;
		built.Maximum = expanded.Maximum;
		built.VertexCount = static_cast<uint32_t>(mesh.Positions.size());

		std::array<std::vector<float>, 3> values;
		values[0].reserve(mesh.Positions.size() * 3);
		values[1].reserve(mesh.Normals.size() * 3);
		values[2].reserve(mesh.UVs.size() * 2);
		for (const engine::core::Vector3 &value : mesh.Positions) {
			values[0].push_back(value.X);
			values[0].push_back(value.Y);
			values[0].push_back(value.Z);
		}
		for (const engine::core::Vector3 &value : mesh.Normals) {
			values[1].push_back(value.X);
			values[1].push_back(value.Y);
			values[1].push_back(value.Z);
		}
		for (const engine::core::Vector2 &value : mesh.UVs) {
			values[2].push_back(value.X);
			values[2].push_back(value.Y);
		}

		const engine::scene::EditablePackingAttribute attributes[] = {
			engine::scene::EditablePackingAttribute::Position,
			engine::scene::EditablePackingAttribute::Normal,
			engine::scene::EditablePackingAttribute::UV,
		};
		const uint32_t components[] = {3, 3, 2};
		for (size_t index = 0; index < built.Streams.size(); ++index) {
			while (built.Vertices.size() % 4 != 0)
				built.Vertices.push_back(std::byte{0});
			engine::scene::EditablePacking policy = mesh.Packing;
			if ((policy.Attributes & static_cast<uint8_t>(attributes[index])) == 0)
				policy.Format = engine::scene::EditablePackingFormat::Float32;
			std::vector<std::byte> encoded;
			if (!engine::scene::PackEditableValues(values[index], policy, encoded)) return {};
			PackedMeshStream &stream = built.Streams[index];
			stream.ByteOffset = static_cast<uint32_t>(built.Vertices.size());
			stream.ByteCount = static_cast<uint32_t>(encoded.size());
			stream.ValueCount = static_cast<uint32_t>(values[index].size());
			stream.Components = components[index];
			stream.Format = PackedFormatOf(policy.Format);
			stream.Minimum = policy.Minimum;
			stream.Maximum = policy.Maximum;
			built.Vertices.insert(built.Vertices.end(), encoded.begin(), encoded.end());
		}
		while (built.Vertices.size() % 4 != 0)
			built.Vertices.push_back(std::byte{0});
		return built.IsValid() ? built : PackedMeshData{};
	}

	size_t EditableMeshUploader::Refresh(
		engine::ecs::Store &store, engine::render::Renderer &renderer, core::Name owner
	) {
		auto foundScope = std::find_if(Scopes.begin(), Scopes.end(), [&](const UploadScope &scope) {
			return scope.World == store.Identity() && scope.Owner == owner;
		});
		if (foundScope == Scopes.end()) {
			Scopes.push_back({store.Identity(), owner, {}});
			foundScope = std::prev(Scopes.end());
		}
		auto &uploadedRevisions = foundScope->Revisions;
		size_t uploaded = 0;

		store.Each<const engine::scene::EditableMesh>([&](engine::ecs::Entity entity,
														  const engine::scene::EditableMesh &mesh) {
			const UploadScope::Revision revision{mesh.Revision, mesh.Packing.Revision};
			const auto found = uploadedRevisions.find(entity.Id);
			if (found != uploadedRevisions.end() && found->second == revision) {
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
				uploadedRevisions[entity.Id] = revision;
				return;
			}

			const engine::core::Name name = engine::scene::EditableMeshContentName(store, entity);
			const bool accepted = mesh.Packing.Enabled()
									  ? renderer.AddPackedMesh(name, BuildPackedMeshData(mesh), owner)
									  : renderer.AddMesh(name, built, owner);
			if (accepted) {
				uploadedRevisions[entity.Id] = revision;
				uploaded++;
			}
		});

		return uploaded;
	}
	void EditableMeshUploader::ForgetWorld(uint64_t identity) {
		std::erase_if(Scopes, [identity](const UploadScope &scope) { return scope.World == identity; });
	}

	void EditableMeshUploader::ForgetOwner(core::Name owner) {
		std::erase_if(Scopes, [owner](const UploadScope &scope) { return scope.Owner == owner; });
	}

}
