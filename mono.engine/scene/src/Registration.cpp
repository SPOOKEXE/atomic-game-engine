#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Animation.hpp>
#include <engine/scene/Atmosphere.hpp>
#include <engine/scene/Audio.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/CollisionShapes.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Constraints.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/EditableMesh.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/LevelOfDetail.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scene/Skinning.hpp>
#include <engine/scene/Sunlight.hpp>
#include <engine/scene/SurfaceCameras.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/scene/Teams.hpp>
#include <engine/scene/Terrain.hpp>
#include <engine/scene/TextureCatalogue.hpp>
#include <engine/scene/Tools.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/scene/Wire.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	namespace {
		// `Surface`, `Visual` and `SurfaceTable` all hold a `core::Name`, and a
		// name's id is a counter this process assigned in first-seen order. The
		// raw object representation would write that counter, and a reading
		// process would resolve it to whatever string happened to take the same
		// number - which is not a corrupt file, it is a file that loads and is
		// wrong. So each of them is written as text.

		void WriteSurfaces(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *surfaces = static_cast<const Surface *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(surfaces[index].Material);
			}
		}

		void ReadSurfaces(core::ByteReader &reader, void *destination, size_t count) {
			auto *surfaces = static_cast<Surface *>(destination);
			for (size_t index = 0; index < count; index++) {
				surfaces[index].Material = reader.ReadName();
			}
		}

		// The four forward-API types that hold a `core::Name`, each written as
		// text for the reason this file opens with. A rig, a clip, a level ladder
		// and a terrain recipe all cross a save file and a wire, and a name's id
		// is a counter this process assigned in first-seen order.

		void WriteSkeletons(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *rigs = static_cast<const Skeleton *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(rigs[index].Rig);
				writer.WriteUInt16(rigs[index].JointCount);
			}
		}

		void ReadSkeletons(core::ByteReader &reader, void *destination, size_t count) {
			auto *rigs = static_cast<Skeleton *>(destination);
			for (size_t index = 0; index < count; index++) {
				rigs[index].Rig = reader.ReadName();
				rigs[index].JointCount = reader.ReadUInt16();
			}
		}

		void WriteAnimationClips(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *clips = static_cast<const AnimationClip *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(clips[index].Asset);
				writer.WriteName(clips[index].Rig);
			}
		}

		void ReadAnimationClips(core::ByteReader &reader, void *destination, size_t count) {
			auto *clips = static_cast<AnimationClip *>(destination);
			for (size_t index = 0; index < count; index++) {
				clips[index].Asset = reader.ReadName();
				clips[index].Rig = reader.ReadName();
			}
		}

		// **Every field written, and the ladder's three names first.**
		// `WriteVisuals` records what a hand-written pair costs: a field added to
		// a type with one crosses only if somebody remembers, and nothing in the
		// build checks. Six fields today, and all six are written.
		void WriteLevelsOfDetail(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *ladders = static_cast<const LevelOfDetail *>(source);
			for (size_t index = 0; index < count; index++) {
				const LevelOfDetail &ladder = ladders[index];
				for (size_t level = 0; level < LOD_LEVELS - 1; level++) {
					writer.WriteName(ladder.Meshes[level]);
					writer.WriteFloat(ladder.Ratios[level]);
				}
				writer.WriteFloat(ladder.TargetQuadArea);
				writer.WriteUInt8(static_cast<uint8_t>(ladder.Strategy));
				writer.WriteUInt8(ladder.Levels);
			}
		}

		void ReadLevelsOfDetail(core::ByteReader &reader, void *destination, size_t count) {
			auto *ladders = static_cast<LevelOfDetail *>(destination);
			for (size_t index = 0; index < count; index++) {
				LevelOfDetail &ladder = ladders[index];
				for (size_t level = 0; level < LOD_LEVELS - 1; level++) {
					ladder.Meshes[level] = reader.ReadName();
					ladder.Ratios[level] = reader.ReadFloat();
				}
				ladder.TargetQuadArea = reader.ReadFloat();

				// Clamped on read rather than trusted, because a strategy past the
				// end of the enum is a `switch` falling through to whatever the
				// compiler chose. `SelectLevel` clamps `Levels` itself for the
				// same reason, so this only has to keep the enum honest.
				const uint8_t strategy = reader.ReadUInt8();
				ladder.Strategy = strategy <= static_cast<uint8_t>(LodStrategy::Reduced)
									  ? static_cast<LodStrategy>(strategy)
									  : LodStrategy::None;
				ladder.Levels = reader.ReadUInt8();
			}
		}

		void WriteTerrains(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *recipes = static_cast<const Terrain *>(source);
			for (size_t index = 0; index < count; index++) {
				const Terrain &recipe = recipes[index];
				writer.WriteUInt64(recipe.Seed);
				writer.WriteName(recipe.Generator);
				writer.WriteFloat(recipe.ChunkExtent);
				writer.WriteFloat(recipe.VerticalExtent);
				writer.WriteFloat(recipe.ViewDistance);
				writer.WriteUInt16(recipe.ChunkResolution);
				writer.WriteBool(recipe.Enabled);
			}
		}

		void ReadTerrains(core::ByteReader &reader, void *destination, size_t count) {
			auto *recipes = static_cast<Terrain *>(destination);
			for (size_t index = 0; index < count; index++) {
				Terrain &recipe = recipes[index];
				recipe.Seed = reader.ReadUInt64();
				recipe.Generator = reader.ReadName();
				recipe.ChunkExtent = reader.ReadFloat();
				recipe.VerticalExtent = reader.ReadFloat();
				recipe.ViewDistance = reader.ReadFloat();
				recipe.ChunkResolution = reader.ReadUInt16();
				recipe.Enabled = reader.ReadBool();
			}
		}

		void WriteTexts(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *texts = static_cast<const TextContent *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteString(texts[index].Value);
			}
		}

		void ReadTexts(core::ByteReader &reader, void *destination, size_t count) {
			auto *texts = static_cast<TextContent *>(destination);
			for (size_t index = 0; index < count; index++) {
				texts[index].Value = std::string(reader.ReadString());
			}
		}

		// `PlayerIdentity::DisplayName` is a `core::Name`, so this pair exists
		// for the reason the paragraph at the top of this file gives: the raw
		// object representation would write the id this process happened to
		// assign, and a reading process would resolve it to whatever string took
		// the same number. A player called somebody else is a file that loads
		// and is wrong.
		// A tick's worth of arrivals and departures, drained by whoever fires
		// signals - so a saved world holding last session's list would deliver a
		// `CharacterAdded` for a body that no longer exists. Written as nothing
		// and read back empty, which is `RenderedSignature`'s pair.
		void WriteCharacterChanges(core::ByteWriter &, const void *, size_t) {}

		void ReadCharacterChanges(core::ByteReader &, void *destination, size_t count) {
			auto *changes = static_cast<CharacterChanges *>(destination);
			for (size_t index = 0; index < count; index++) {
				changes[index] = CharacterChanges{};
			}
		}

		void WritePlayerIdentities(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *identities = static_cast<const PlayerIdentity *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteInt64(identities[index].UserId);
				writer.WriteName(identities[index].DisplayName);
				writer.WriteFloat(identities[index].RespawnTime);
			}
		}

		void ReadPlayerIdentities(core::ByteReader &reader, void *destination, size_t count) {
			auto *identities = static_cast<PlayerIdentity *>(destination);
			for (size_t index = 0; index < count; index++) {
				identities[index].UserId = reader.ReadInt64();
				identities[index].DisplayName = reader.ReadName();
				identities[index].RespawnTime = reader.ReadFloat();
			}
		}

		void WriteVisuals(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *visuals = static_cast<const Visual *>(source);
			for (size_t index = 0; index < count; index++) {
				const Visual &visual = visuals[index];
				writer.WriteFloat(visual.Tint.R);
				writer.WriteFloat(visual.Tint.G);
				writer.WriteFloat(visual.Tint.B);
				writer.WriteName(visual.Mesh);
				writer.WriteBool(visual.Visible);

				// **Written, and forgetting to would be this function's own
				// documented failure mode repeating.** See the paragraph below:
				// two fields added to `Visual` crossed for free everywhere except
				// here and silently reset on every load. A part whose `Fitted`
				// reset would be reshaped the next time its mesh arrived.
				writer.WriteName(visual.Fitted);

				// **Added at v0.7, and the reason it was missing is the reason
				// a custom serialiser is dangerous.** A field added to a type
				// with a generated serialiser crosses for free; a field added to
				// one with a hand-written pair crosses only if somebody
				// remembers, and nothing in the build checks. `Transparency` and
				// `Surface` both landed after this function was written and
				// both silently reset to their defaults on every load -
				// invisible for a part that was opaque anyway, and a glass pane
				// that turned solid the first time a world was saved and
				// reopened. `mono.client/tests/Presentation.cpp` is the check
				// that would have caught it, and now does.
				writer.WriteFloat(visual.Transparency);

				// **Sixteen bits, because the field is sixteen bits.** It was
				// written and read as an `int8_t` from v0.7 to v0.19, and
				// `Visual::Surface` was widened to `int16_t` at v0.17
				// *expressly* to lift a ceiling of a hundred and twenty-seven
				// mirrors - so every slot index from 128 up was silently
				// truncated on the way into a file and read back as some other
				// pane, or as -1. The paragraph above is about a field that
				// crosses only if somebody remembers; this is the same failure
				// one step further on, where somebody remembered the field and
				// not its width.
				writer.WriteInt16(visual.Surface);

				// Added at v0.7 beside the shadow pass that reads it, and
				// written here in the same breath rather than a release later -
				// which is the whole lesson of the paragraph above.
				writer.WriteBool(visual.CastShadow);

				// Added at v0.12, in the same breath, for the same reason. A
				// part somebody locked and then saved would otherwise come back
				// grabbable, which is the one thing locking it was for.
				writer.WriteBool(visual.Locked);
			}
		}

		void ReadVisuals(core::ByteReader &reader, void *destination, size_t count) {
			auto *visuals = static_cast<Visual *>(destination);
			for (size_t index = 0; index < count; index++) {
				Visual &visual = visuals[index];
				visual.Tint.R = reader.ReadFloat();
				visual.Tint.G = reader.ReadFloat();
				visual.Tint.B = reader.ReadFloat();
				visual.Mesh = reader.ReadName();
				visual.Visible = reader.ReadBool();
				visual.Fitted = reader.ReadName();
				visual.Transparency = reader.ReadFloat();
				visual.Surface = reader.ReadInt16();
				visual.CastShadow = reader.ReadBool();
				visual.Locked = reader.ReadBool();
			}
		}

		// **A `LocalTransparency` is a fact about this viewer, and its
		// serialisation says so by writing nothing.**
		//
		// A saved world holding last session's camera-occlusion fade would
		// reopen with a wall standing there permanently half-transparent, for
		// a poppercam that has not run a single frame yet - the same shape of
		// bug `RenderedSignature`'s own comment warns against, one row up. The
		// reader resets every row to the default rather than reading any bytes
		// at all, so a fresh load always starts from "nothing is faded" and
		// the very next camera pass decides the truth from there.
		void WriteLocalTransparencies(core::ByteWriter &, const void *, size_t) {}

		void ReadLocalTransparencies(core::ByteReader &, void *destination, size_t count) {
			auto *values = static_cast<LocalTransparency *>(destination);
			for (size_t index = 0; index < count; index++) {
				values[index] = LocalTransparency{};
			}
		}

		// **A `RenderedSignature` is derived state, and its serialisation says
		// so by writing nothing.**
		//
		// It is not merely redundant to save - it is dangerous. The stamp says
		// "the walk has already been run against a tree that folds to this",
		// and a world restored from a snapshot has had no walk run against it
		// at all. Carrying a live value across a load would let the very first
		// `SyncRendered` match, skip, and leave the tag as whatever the file
		// happened to hold. The symptom is a loaded game that renders wrong
		// once, and nothing in the frame it renders wrong in explains why.
		//
		// So the reader zeroes the whole thing rather than reading anything:
		// `Fresh` back to zero is what forces the first sync after a load to be
		// a real one. `client::DrawList` writes nothing for the related but
		// weaker reason that its value is recomputed before anybody looks.
		void WriteRenderedSignatures(core::ByteWriter &, const void *, size_t) {}

		void ReadRenderedSignatures(core::ByteReader &, void *destination, size_t count) {
			auto *memos = static_cast<RenderedSignature *>(destination);
			for (size_t index = 0; index < count; index++) {
				memos[index] = RenderedSignature{};
			}
		}

		// Derived resource: do not persist stale mesh counts.
		void WriteMeshCatalogues(core::ByteWriter &, const void *, size_t) {}

		void ReadMeshCatalogues(core::ByteReader &, void *destination, size_t count) {
			auto *catalogues = static_cast<MeshCatalogue *>(destination);
			for (size_t index = 0; index < count; index++) {
				catalogues[index].Triangles.clear();
			}
		}

		// Derived resource, the same as the mesh one above and for the same
		// reason: the frame counts came from whatever registered the textures
		// this run, and a save file carrying last run's would be numbers that
		// agree with nothing on disk.
		void WriteTextureCatalogues(core::ByteWriter &, const void *, size_t) {}

		void ReadTextureCatalogues(core::ByteReader &, void *destination, size_t count) {
			auto *catalogues = static_cast<TextureCatalogue *>(destination);
			for (size_t index = 0; index < count; index++) {
				catalogues[index].Flipbooks.clear();
				catalogues[index].Revision = 0;
			}
		}

		// Derived resource, the same as the ones above. **And the most obviously
		// so of the four**: this list came from a manifest a publisher signed and
		// a client verified *this run*, so a save file carrying last run's would
		// offer a scene names that nothing in the store answers to.
		void WritePublishedCatalogues(core::ByteWriter &, const void *, size_t) {}

		void ReadPublishedCatalogues(core::ByteReader &, void *destination, size_t count) {
			auto *catalogues = static_cast<PublishedCatalogue *>(destination);
			for (size_t index = 0; index < count; index++) {
				catalogues[index].Meshes.clear();
			}
		}

		// Derived resource, the same as the two above and for the same reason:
		// the texture names came from whatever registered the materials this run,
		// and a save file carrying last run's would be names that resolve to
		// nothing on a machine with different content.
		void WriteMaterialCatalogues(core::ByteWriter &, const void *, size_t) {}

		void ReadMaterialCatalogues(core::ByteReader &, void *destination, size_t count) {
			auto *catalogues = static_cast<MaterialCatalogue *>(destination);
			for (size_t index = 0; index < count; index++) {
				catalogues[index].ColourMaps.clear();

				// **The resolved set has to go with it, and this one is not
				// merely tidy.** It holds entity handles, and a load replaces
				// the whole directory - so a handle kept across it names
				// whatever now sits at that index, and the next pass would
				// clear the colour map of an unrelated part exactly once,
				// immediately after loading.
				catalogues[index].Resolved.clear();
			}
		}

		// A name, so a hand-written pair. The rule this file opens with.
		void WriteMaterialRefs(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *refs = static_cast<const MaterialRef *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(refs[index].Asset);

				// Written the day it was added, which is `WriteVisuals`' lesson:
				// a material whose shader reset on every load would be a part
				// that came back drawn by the engine's default and nothing in
				// the file saying why.
				writer.WriteName(refs[index].Shader);
			}
		}

		void ReadMaterialRefs(core::ByteReader &reader, void *destination, size_t count) {
			auto *refs = static_cast<MaterialRef *>(destination);
			for (size_t index = 0; index < count; index++) {
				refs[index].Asset = reader.ReadName();
				refs[index].Shader = reader.ReadName();
			}
		}

		// **A written pair rather than the generated one**, for `WriteTexts`'
		// reason: `ShaderSource` holds a `std::string` and is therefore not
		// trivially copyable, so there is no object representation to write.
		void WriteShaderSources(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *sources = static_cast<const ShaderSource *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteString(sources[index].Code);

				// **The revision travels with the code.** A library that
				// restored the text and reset the counter would believe a
				// reloaded world's shaders were the ones it had already
				// compiled, and would draw last session's.
				writer.WriteUInt32(sources[index].Revision);
			}
		}

		void ReadShaderSources(core::ByteReader &reader, void *destination, size_t count) {
			auto *sources = static_cast<ShaderSource *>(destination);
			for (size_t index = 0; index < count; index++) {
				sources[index].Code = std::string(reader.ReadString());
				sources[index].Revision = reader.ReadUInt32();
			}
		}

		// **A written pair for `WriteShaderSources`' reason**: five
		// `std::vector`s hold no object representation a raw copy could
		// write. The vertex count is written once and read back to size
		// every array the same way, rather than once per array, because the
		// four are parallel by construction - `EditableMesh`'s own header
		// carries that invariant.
		void WriteEditableMeshes(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *meshes = static_cast<const EditableMesh *>(source);
			for (size_t index = 0; index < count; index++) {
				const EditableMesh &mesh = meshes[index];
				const auto vertices = static_cast<uint32_t>(mesh.Positions.size());
				writer.WriteUInt32(vertices);
				for (uint32_t vertex = 0; vertex < vertices; vertex++) {
					writer.WriteFloat(mesh.Positions[vertex].X);
					writer.WriteFloat(mesh.Positions[vertex].Y);
					writer.WriteFloat(mesh.Positions[vertex].Z);
					writer.WriteFloat(mesh.Normals[vertex].X);
					writer.WriteFloat(mesh.Normals[vertex].Y);
					writer.WriteFloat(mesh.Normals[vertex].Z);
					writer.WriteFloat(mesh.UVs[vertex].X);
					writer.WriteFloat(mesh.UVs[vertex].Y);
					writer.WriteFloat(mesh.Colours[vertex].R);
					writer.WriteFloat(mesh.Colours[vertex].G);
					writer.WriteFloat(mesh.Colours[vertex].B);
					writer.WriteFloat(mesh.Alphas[vertex]);
				}

				const auto indices = static_cast<uint32_t>(mesh.Indices.size());
				writer.WriteUInt32(indices);
				for (uint32_t entry = 0; entry < indices; entry++) {
					writer.WriteUInt32(mesh.Indices[entry]);
				}

				// The revision travels with the geometry, for
				// `WriteShaderSources`' identical reason: a reader that reset
				// it would believe a reloaded mesh was one already uploaded.
				writer.WriteUInt32(mesh.Revision);
			}
		}

		void ReadEditableMeshes(core::ByteReader &reader, void *destination, size_t count) {
			auto *meshes = static_cast<EditableMesh *>(destination);
			for (size_t index = 0; index < count; index++) {
				EditableMesh &mesh = meshes[index];
				mesh.Positions.clear();
				mesh.Normals.clear();
				mesh.UVs.clear();
				mesh.Colours.clear();
				mesh.Alphas.clear();
				mesh.Indices.clear();

				const uint32_t vertices = reader.ReadUInt32();
				mesh.Positions.reserve(vertices);
				mesh.Normals.reserve(vertices);
				mesh.UVs.reserve(vertices);
				mesh.Colours.reserve(vertices);
				mesh.Alphas.reserve(vertices);
				for (uint32_t vertex = 0; vertex < vertices; vertex++) {
					core::Vector3 position;
					position.X = reader.ReadFloat();
					position.Y = reader.ReadFloat();
					position.Z = reader.ReadFloat();
					mesh.Positions.push_back(position);

					core::Vector3 normal;
					normal.X = reader.ReadFloat();
					normal.Y = reader.ReadFloat();
					normal.Z = reader.ReadFloat();
					mesh.Normals.push_back(normal);

					core::Vector2 uv;
					uv.X = reader.ReadFloat();
					uv.Y = reader.ReadFloat();
					mesh.UVs.push_back(uv);

					core::Color3 colour;
					colour.R = reader.ReadFloat();
					colour.G = reader.ReadFloat();
					colour.B = reader.ReadFloat();
					mesh.Colours.push_back(colour);

					mesh.Alphas.push_back(reader.ReadFloat());
				}

				const uint32_t indices = reader.ReadUInt32();
				mesh.Indices.reserve(indices);
				for (uint32_t entry = 0; entry < indices; entry++) {
					mesh.Indices.push_back(reader.ReadUInt32());
				}

				mesh.Revision = reader.ReadUInt32();
			}
		}

		// **A written pair for `WriteEditableMeshes`' identical reason.**
		// Width and height are written explicitly rather than derived from
		// the buffer's length, so a corrupt file that lied about one is
		// caught the moment the two disagree with `Pixels.size()` rather
		// than read past the end of a shorter buffer.
		void WriteEditableImages(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *images = static_cast<const EditableImage *>(source);
			for (size_t index = 0; index < count; index++) {
				const EditableImage &image = images[index];
				writer.WriteUInt32(image.Width);
				writer.WriteUInt32(image.Height);
				writer.WriteUInt32(static_cast<uint32_t>(image.Pixels.size()));
				if (!image.Pixels.empty()) {
					writer.WriteRaw(image.Pixels.data(), image.Pixels.size());
				}
				writer.WriteUInt32(image.Revision);
			}
		}

		void ReadEditableImages(core::ByteReader &reader, void *destination, size_t count) {
			auto *images = static_cast<EditableImage *>(destination);
			for (size_t index = 0; index < count; index++) {
				EditableImage &image = images[index];
				image.Width = reader.ReadUInt32();
				image.Height = reader.ReadUInt32();
				const uint32_t bytes = reader.ReadUInt32();
				image.Pixels.assign(bytes, 0);
				if (bytes > 0) {
					reader.ReadRaw(image.Pixels.data(), bytes);
				}
				image.Revision = reader.ReadUInt32();
			}
		}

		// A written pair for `WriteMaterialRefs`' reason: `PostProcessing`
		// holds a `core::Name`, and the raw object representation is a
		// process-local id rather than the text it names.
		void WritePostProcessing(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *settings = static_cast<const PostProcessing *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(settings[index].Shader);
			}
		}

		void ReadPostProcessing(core::ByteReader &reader, void *destination, size_t count) {
			auto *settings = static_cast<PostProcessing *>(destination);
			for (size_t index = 0; index < count; index++) {
				settings[index].Shader = reader.ReadName();
			}
		}

		void WriteSurfaceAppearances(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *appearances = static_cast<const SurfaceAppearance *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteName(appearances[index].ColourMap);

				// **Written the day they are added**, which is the lesson
				// `WriteVisuals` records: a field that travels a release later
				// than the struct it is in is a world that loads with the field
				// silently empty and nothing saying so.
				writer.WriteName(appearances[index].NormalMap);
				writer.WriteName(appearances[index].RoughnessMap);
				writer.WriteName(appearances[index].OcclusionMap);
				writer.WriteName(appearances[index].HeightMap);
				writer.WriteName(appearances[index].EmissiveMap);
				writer.WriteName(appearances[index].Shader);

				writer.WriteFloat(appearances[index].AlphaCutoff);
				writer.WriteUInt8(static_cast<uint8_t>(appearances[index].Mode));

				// Appended so prior field offsets never move.
				writer.WriteName(appearances[index].MetalnessMap);
				writer.WriteFloat(appearances[index].Colour.R);
				writer.WriteFloat(appearances[index].Colour.G);
				writer.WriteFloat(appearances[index].Colour.B);
				writer.WriteFloat(appearances[index].EmissiveTint.R);
				writer.WriteFloat(appearances[index].EmissiveTint.G);
				writer.WriteFloat(appearances[index].EmissiveTint.B);
				writer.WriteFloat(appearances[index].EmissiveStrength);
				writer.WriteUInt8(static_cast<uint8_t>(appearances[index].Resample));
			}
		}

		// **A hand-written pair, because it holds a name**, and every field is
		// written the day it is added rather than a release later - the lesson
		// `WriteVisuals` records above, applied from this function's first line.
		void WriteSounds(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *sounds = static_cast<const Sound *>(source);
			for (size_t index = 0; index < count; index++) {
				const Sound &sound = sounds[index];
				writer.WriteName(sound.SoundId);
				writer.WriteFloat(sound.Volume);
				writer.WriteFloat(sound.RollOffMinDistance);
				writer.WriteFloat(sound.RollOffMaxDistance);
				writer.WriteBool(sound.Looped);

				// **Written, so a save file remembers what was playing.** The
				// alternative reads as tidier - a loaded world starts silent -
				// and is the wrong default: a level whose ambience is a looping
				// `Sound` under `Workspace` would come back mute, and nothing
				// in the file would say why.
				writer.WriteBool(sound.Playing);
			}
		}

		void ReadSounds(core::ByteReader &reader, void *destination, size_t count) {
			auto *sounds = static_cast<Sound *>(destination);
			for (size_t index = 0; index < count; index++) {
				Sound &sound = sounds[index];
				sound.SoundId = reader.ReadName();
				sound.Volume = reader.ReadFloat();
				sound.RollOffMinDistance = reader.ReadFloat();
				sound.RollOffMaxDistance = reader.ReadFloat();
				sound.Looped = reader.ReadBool();
				sound.Playing = reader.ReadBool();
			}
		}

		void ReadSurfaceAppearances(core::ByteReader &reader, void *destination, size_t count) {
			auto *appearances = static_cast<SurfaceAppearance *>(destination);
			for (size_t index = 0; index < count; index++) {
				appearances[index].ColourMap = reader.ReadName();
				appearances[index].NormalMap = reader.ReadName();
				appearances[index].RoughnessMap = reader.ReadName();
				appearances[index].OcclusionMap = reader.ReadName();
				appearances[index].HeightMap = reader.ReadName();
				appearances[index].EmissiveMap = reader.ReadName();
				appearances[index].Shader = reader.ReadName();
				appearances[index].AlphaCutoff = reader.ReadFloat();

				const uint8_t mode = reader.ReadUInt8();

				// Range-checked before the cast, for `assets::Texture::Read`'s
				// reason: a cast of an out-of-range byte produces a value no
				// switch handles, and every consumer downstream then reads
				// something the type says cannot exist.
				appearances[index].Mode = mode <= static_cast<uint8_t>(AlphaMode::Opaque)
											  ? static_cast<AlphaMode>(mode)
											  : AlphaMode::Opaque;
				appearances[index].MetalnessMap = reader.ReadName();
				appearances[index].Colour = {reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()};
				appearances[index].EmissiveTint = {
					reader.ReadFloat(), reader.ReadFloat(), reader.ReadFloat()
				};
				appearances[index].EmissiveStrength = reader.ReadFloat();
				const uint8_t resample = reader.ReadUInt8();
				appearances[index].Resample = resample <= static_cast<uint8_t>(SurfaceResampleMode::Pixelated)
												  ? static_cast<SurfaceResampleMode>(resample)
												  : SurfaceResampleMode::Default;
			}
		}

		// **The tag names, and they have to travel with the masks.** A `Tags`
		// component is a bare integer whose bits mean whatever this table says
		// they mean, so a world restored with the masks and without the table
		// would have every tagged object in a group with no name - and a surface
		// camera filtering by name would match nothing, silently.
		void WriteTagTables(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *tables = static_cast<const TagTable *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteUInt32(static_cast<uint32_t>(tables[index].Names.size()));

				// In registration order, because the index *is* the bit. Sorting
				// here would renumber every mask already stored on a row -
				// `WriteSurfaceTables` says the same about its own.
				for (const core::Name &name : tables[index].Names) {
					writer.WriteName(name);
				}
			}
		}

		void ReadTagTables(core::ByteReader &reader, void *destination, size_t count) {
			auto *tables = static_cast<TagTable *>(destination);
			for (size_t index = 0; index < count; index++) {
				tables[index].Names.clear();

				const uint32_t names = reader.ReadUInt32();
				for (uint32_t at = 0; at < names && !reader.Failed(); at++) {
					if (tables[index].Names.size() >= TagTable::MAXIMUM) {
						// A file claiming more tags than a mask has bits. Dropped
						// rather than read, because a bit past thirty-two cannot be
						// referenced by any mask anyway.
						break;
					}
					tables[index].Names.push_back(reader.ReadName());
				}
			}
		}

		void WriteSurfaceTables(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *tables = static_cast<const SurfaceTable *>(source);
			for (size_t index = 0; index < count; index++) {
				const SurfaceTable &table = tables[index];
				writer.WriteUInt32(static_cast<uint32_t>(table.Rows.size()));

				// In stored order rather than sorted. The order is program
				// order - the sequence the scene registered materials in - so
				// two runs of one scene write identical bytes, and sorting here
				// would only hide a table that had been built differently.
				for (const SurfaceRow &row : table.Rows) {
					writer.WriteName(row.Material);
					writer.WriteFloat(row.Properties.Friction);
					writer.WriteFloat(row.Properties.Restitution);
				}
			}
		}

		void ReadSurfaceTables(core::ByteReader &reader, void *destination, size_t count) {
			auto *tables = static_cast<SurfaceTable *>(destination);
			for (size_t index = 0; index < count; index++) {
				SurfaceTable &table = tables[index];
				table.Rows.clear();

				const uint32_t rows = reader.ReadUInt32();
				for (uint32_t at = 0; at < rows && !reader.Failed(); at++) {
					SurfaceRow row;
					row.Material = reader.ReadName();
					row.Properties.Friction = reader.ReadFloat();
					row.Properties.Restitution = reader.ReadFloat();
					table.Rows.push_back(row);
				}
			}
		}
	}

	void RegisterSceneComponents() {
		// Order is deliberate and not alphabetical: it is the order the plan
		// lists them in, and it decides component ids, which decide the order
		// archetypes iterate their columns in. Reordering these lines changes
		// the order rows are visited across the whole engine, so a
		// floating-point sum over them can come out differently. Add at the
		// end.
		// **The two that carry a wire form are the two that dominate a delta**,
		// and this is the only place either half of the engine says so - a
		// server and a client cannot disagree about how a `Transform` crosses
		// without disagreeing about what a `Transform` is. The file
		// serialisation is untouched and stays lossless; `Wire.hpp` says what
		// the compact one costs and `ecs/TypeDescriptor.hpp` says why it is a
		// second pair rather than a replacement.
		//
		// `PreviousTransform` deliberately has none. It is a render-side
		// history nothing replicates, and giving it one would be declaring a
		// wire form for something that never reaches a wire.
		// **The tree, and it is registered here for want of an earlier place
		// that every host already calls.** `ecs::Hierarchy` is the ECS's own
		// type and had only the automatic name `Components::Of<T>` mints from
		// the compiler's spelling - which is unusable on a wire, because the two
		// processes have to agree on it and nothing makes them.
		//
		// It has to happen before anything creates an instance:
		// `Components::Of<T>` caches its answer per type per process, and
		// `Adopt` aborts on an explicit registration that arrives after an
		// automatic one under a different name. This function is the earliest
		// call every program makes, which is why it is here rather than beside
		// the type.
		//
		// **The default POD wire form, and that is safe because a `Hierarchy` is
		// five entity handles and nothing else.** A replica adopts the
		// authority's indices - `Store::SetAdoptOnly` exists to guarantee it -
		// so the handles mean the same thing on both ends without remapping.
		//
		// A child whose parent has not arrived yet holds a handle to a row that
		// does not exist. `Instances.cpp` resolves every link through
		// `MutableNodeOf`, which returns null for a missing row, so the walk
		// stops early and resumes when the next delta fills the gap. Transient,
		// and stated because the alternative reading is a corrupted tree.
		// `ecs.Hierarchy` used to be registered here, and it was the wrong
		// place: an `ecs::` type named by `scene` is a name that depends on this
		// function having run first, and nothing made it. `ecs` names all three
		// instance components itself now - see `RegisterInstanceComponents`.

		// Added at the end of this list rather than beside `ServiceComponent`,
		// per the ordering note above: a component id decides column order, and
		// inserting one in the middle reorders iteration across the engine.
		// **`Sun` is a resource nothing registered, and it registered itself on
		// first read.** A world's directional light is a per-world resource, so
		// the first `store.Resource<Sun>()` minted an id under the compiler's
		// spelling of the type - the same failure `client::DrawList` had at v0.7
		// and `physics::PoppercamState` had until v0.19. It reaches a `.agame`,
		// which is rule 4: a name that crosses a file is a string somebody
		// chose.
		//
		// Found by sealing the table and running all forty-three example scenes;
		// it was the only one left after the other four were named.
		ecs::Components::Register<Sun>("scene.Sun");

		// **`PortalProxy` had no name until v0.19, and that made a live rule
		// dead.** `replication/src/Defaults.cpp` has tested for
		// `"scene.PortalProxy"` in `LocalToTheClient` since portals landed, and
		// the component was registering under the compiler's spelling of the
		// type - so the string never matched and every proxy was replicated.
		// The comment beside that test says what it costs: a proxy is made and
		// unmade inside one tick, so replicating one is a create and a destroy
		// per proxy per tick on the wire, describing geometry the client
		// already has on the other side of the pane.
		//
		// Named here, so the rule that was always meant to apply now does.
		ecs::Components::Register<PortalProxy>("scene.PortalProxy");

		ecs::Components::Register<Transform>("scene.Transform", TransformWire());
		ecs::Components::Register<PreviousTransform>("scene.PreviousTransform");
		ecs::Components::Register<Bounds>("scene.Bounds");
		ecs::Components::Register<Motion>("scene.Motion", MotionWire());
		ecs::Components::Register<RigidBody>("scene.RigidBody");

		// A tag, so it costs a column of nothing and crosses as presence. The
		// authority decides whether a part is simulated, so it crosses like
		// every other `scene.` row.
		//
		// **The name changed with the polarity at v0.18**, and a snapshot or an
		// `.agame` written before that reads wrong rather than failing: the rows
		// that carried `scene.Anchored` are exactly the ones that should now
		// carry nothing, and the name is simply absent from this table. Nothing
		// migrates it, because the format is pre-release and `docs/RELEASING.md`
		// says what that means.
		ecs::Components::Register<Simulated>("scene.Simulated");
		ecs::Components::Register<Collider>("scene.Collider");
		ecs::Components::Register<Surface>("scene.Surface", WriteSurfaces, ReadSurfaces);

		// **After `Surface`, because it overrides what `Surface` resolves to**,
		// and at the end of the physics group rather than in the middle of it:
		// the order here decides component ids and therefore the order columns
		// are visited, so new entries go after the ones that were already there.
		ecs::Components::Register<PhysicsProperties>("scene.PhysicsProperties");
		ecs::Components::Register<Visual>("scene.Visual", WriteVisuals, ReadVisuals);

		// **A hand-written pair, because it holds a name.** The lesson
		// `WriteVisuals` records above applies here from the first line: a
		// field added to a type with a hand-written pair crosses only if
		// somebody remembers, and nothing in the build checks. Three fields
		// today, and all three are written.
		ecs::Components::Register<SurfaceAppearance>(
			"scene.SurfaceAppearance", WriteSurfaceAppearances, ReadSurfaceAppearances
		);

		// **The default POD form, and that is right here.** A `Tags` is one
		// integer and no name - the *names* are in the `TagTable` resource,
		// which is serialised beside it, so the mask and the meanings of its
		// bits travel together or not at all.
		ecs::Components::Register<Tags>("scene.Tags");

		// **A dense column for the same reason `SurfaceAppearance` and `Tags`
		// are, and a hand-written pair for `RenderedSignature`'s.** See
		// `LocalTransparency`'s own header for why the value must not survive
		// a save, and `replication::LocalToTheClient` for why it must not
		// cross the wire despite the `scene.` name every replicated component
		// carries.
		ecs::Components::Register<LocalTransparency>(
			"scene.LocalTransparency", WriteLocalTransparencies, ReadLocalTransparencies
		);
		ecs::Components::Register<SurfaceCamera>("scene.SurfaceCamera");

		// **A written pair rather than the generated one**, because the type
		// holds a `std::string` and is therefore not trivially copyable -
		// `DescribeType` offers raw serialisation only for a type that is.
		ecs::Components::Register<TextContent>("scene.TextContent", WriteTexts, ReadTexts);
		ecs::Components::Register<Camera>("scene.Camera");

		// A name again, so a hand-written pair again. See `WriteSounds`.
		ecs::Components::Register<Sound>("scene.Sound", WriteSounds, ReadSounds);

		// **Both generated, because neither holds a `core::Name`.** The rule this
		// file opens with is about names and only about names: a name's id is a
		// counter this process assigned, so it is written as text. An
		// `Attachment` is two `CFrame`s and a `Light` is a colour, four floats and
		// three bytes of enum and flags - all of which mean the same thing in
		// every process, so the object representation is the format and the
		// generated pair is correct.
		//
		// **The derived half of an `Attachment` crosses too**, and that is worth a
		// sentence rather than a shrug: `WorldFrame` is a cache with one writer,
		// and writing a cache into a snapshot is normally the thing rule 2
		// refuses. It goes in because the alternative is a restored world whose
		// beams draw at the origin for one frame - `ResolveAttachments` runs in
		// `PreRender`, so a load that cleared it would present before it was
		// filled. One frame of a beam in the wrong place is more visible than
		// thirty-two bytes an entity.
		// **Generated, because a `Pivot` is one `CFrame`** - no name, so the
		// object representation is the format. Registered beside `Attachment`
		// for the same reason it is: both are placements relative to something
		// else.
		ecs::Components::Register<Pivot>("scene.Pivot");

		ecs::Components::Register<Attachment>("scene.Attachment");
		ecs::Components::Register<Light>("scene.Light");

		// **Generated, because a `Humanoid` is nine floats and four flags.** No
		// name, so the object representation is the format and there is nothing
		// to hand-write - which is the desirable case and the one most components
		// here are in.
		ecs::Components::Register<Humanoid>("scene.Humanoid");

		// The service fixtures, added at the end because that is the rule this
		// list opens with: registration order decides component ids, ids decide
		// the order archetypes iterate their columns, and inserting one of
		// these above `Transform` would change the order every row in the
		// engine is visited in. Neither carries a wire form - a service is
		// authored content that a snapshot moves, not per-tick state a delta
		// does.
		//
		// `scene.Transient` is a tag, so it costs a column of nothing and a
		// snapshot carries it as the entity's name in the component's row list.
		ecs::Components::Register<TransientComponent>("scene.Transient");
		ecs::Components::Register<ServiceComponent>("scene.Service");
		ecs::Components::Register<LightingServiceComponent>("scene.LightingService");

		// **Who this host is looking through, and only a client has one.** A
		// resource, so it is carried by a snapshot and covered by the affinity
		// check like everything else a world holds - and named explicitly
		// because an automatic name minted from the compiler's spelling is
		// unusable the moment a world crosses a process.
		ecs::Components::Register<LocalPlayer>("scene.LocalPlayer");

		// **What a `Material` instance holds, added at the end** for the reason
		// this list opens with: registration order decides component ids, and ids
		// decide the order archetypes iterate their columns. A hand-written pair
		// because it is a name.
		ecs::Components::Register<MaterialRef>("scene.MaterialRef", WriteMaterialRefs, ReadMaterialRefs);

		// **What a `ShaderScript` instance holds**, registered beside the
		// material that names one and at the end for the same reason. A written
		// pair because it holds a `std::string`, not because it holds a name.
		ecs::Components::Register<ShaderSource>("scene.ShaderSource", WriteShaderSources, ReadShaderSources);

		// **What an `EditableMesh` instance holds, beside the shader source
		// for the same registration-order reason.** A written pair because it
		// holds five `std::vector`s, not because it holds a name.
		ecs::Components::Register<EditableMesh>(
			"scene.EditableMesh", WriteEditableMeshes, ReadEditableMeshes
		);

		// **What an `EditableImage` instance holds, beside its mesh
		// counterpart for the identical reason.** A written pair because it
		// holds a `std::vector<uint8_t>` of raw pixels.
		ecs::Components::Register<EditableImage>(
			"scene.EditableImage", WriteEditableImages, ReadEditableImages
		);

		// The render gate, at the end for the reason this list opens with.
		//
		// **No wire form, and it is not an oversight.** This is a conclusion
		// drawn from the tree, and a conclusion is the thing least worth
		// sending: an authority replicates what is in its own scene, so a
		// replica's draw list is already filtered by what arrived. Putting this
		// on the wire would give a replica a second opinion about visibility
		// that could disagree with the first. `client::CollectReplicated`
		// carries the same argument from the receiving end.
		ecs::Components::Register<Rendered>("scene.Rendered");

		// The resources. A resource is keyed by a component id too, so one that
		// is never registered here would be minted by the first
		// `Store::SetResource` - under the compiler's spelling of the type, and
		// aborting outright once the table is sealed.
		ecs::Components::Register<SurfaceTable>("scene.SurfaceTable", WriteSurfaceTables, ReadSurfaceTables);
		ecs::Components::Register<TagTable>("scene.TagTable", WriteTagTables, ReadTagTables);

		// **The baked collision shapes, and they do not cross.** A hull is
		// derived from content the receiving side already has, so putting it on
		// the wire is sending a conclusion instead of its input - and the
		// conclusion is the half an attacker gets to choose. A shape with a
		// bound that does not match its points is a collider that stops things
		// it is not touching. `PhysicsWorld` makes the same decision for its
		// grids.
		//
		// **A writer that writes nothing rather than no writer at all**, which
		// is `client::DrawList`'s arrangement and is here for its reason:
		// `Store::Save` refuses a resource with no serialisation rather than
		// writing bytes that cannot be read back, so a world holding this could
		// not be snapshotted - and the studio snapshots a universe every time
		// Play is pressed. Nothing crosses either way, so the wire argument
		// above is untouched.
		//
		// **The reader clears**, because what comes back has to be rebuilt
		// rather than inherited: a restored world is handed its shapes again by
		// whichever host restored it - `Editor::PrepareWorldIn`,
		// `Server::InstallCollisionShapes` - out of the content that host has.
		// Keeping a stale table would be keeping the one copy nothing owns.
		//
		// It is registered rather than left to be minted by the first
		// `SetResource`, for the reason this block opens with: an unregistered
		// resource takes the compiler's spelling of its type and aborts once the
		// table is sealed.
		ecs::Components::Register<CollisionShapes>(
			"scene.CollisionShapes",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *tables = static_cast<CollisionShapes *>(destination);
				for (size_t index = 0; index < count; index++) {
					tables[index].Hulls.clear();
					tables[index].Meshes.clear();
				}
			}
		);
		// **The bake ledger beside the table it fills.** Which revision of each
		// `EditableMesh` has a shape baked for it is derived from the meshes
		// themselves, so it is registered with the same pair `CollisionShapes`
		// gets: a writer that writes nothing and a reader that clears. A world
		// restored from a snapshot arrives with an empty shape table and must
		// arrive with an empty ledger too, or the first refresh would compare
		// revisions against shapes that are not there and skip every one of
		// them. See `RefreshEditableMeshCollision`.
		ecs::Components::Register<EditableMeshCollision>(
			"scene.EditableMeshCollision",
			[](core::ByteWriter &, const void *, size_t) {},
			[](core::ByteReader &, void *destination, size_t count) {
				auto *ledgers = static_cast<EditableMeshCollision *>(destination);
				for (size_t index = 0; index < count; index++) {
					ledgers[index].Rows.clear();
				}
			}
		);

		ecs::Components::Register<ActiveCamera>("scene.ActiveCamera");

		// **`InputState` crosses and `CameraController` crosses**, which is worth
		// a sentence because one of them looks like it should not. Input is
		// per-frame state and writing it into a save file sounds like storing an
		// answer that is recomputed - and it is, except that a *recording* is the
		// case that matters: `just replay-check` replays a world from a snapshot
		// and its input stream, and a snapshot whose input state was cleared would
		// replay a character that never moved.
		//
		// The camera controller is authored state - a mode, a zoom, a sensitivity
		// - and crosses for the ordinary reason.
		ecs::Components::Register<InputState>("scene.InputState");

		// **`AudioState` crosses for `CameraController`'s reason and not
		// `InputState`'s.** It is authored state - a master gain and where the ear
		// is - rather than a per-frame report, so a world reopened from a file is
		// as loud as it was left. The plain object representation is enough: an
		// `ecs::Entity` is an index and a generation, which is exactly what
		// `LocalPlayer` writes, and a snapshot that carries the row carries the
		// instance it names.
		ecs::Components::Register<AudioState>("scene.AudioState");
		ecs::Components::Register<CameraController>("scene.CameraController");
		ecs::Components::Register<WorldBounds>("scene.WorldBounds");
		ecs::Components::Register<MeshCatalogue>(
			"scene.MeshCatalogue", WriteMeshCatalogues, ReadMeshCatalogues
		);
		ecs::Components::Register<TextureCatalogue>(
			"scene.TextureCatalogue", WriteTextureCatalogues, ReadTextureCatalogues
		);
		ecs::Components::Register<MaterialCatalogue>(
			"scene.MaterialCatalogue", WriteMaterialCatalogues, ReadMaterialCatalogues
		);
		ecs::Components::Register<PublishedCatalogue>(
			"scene.PublishedCatalogue", WritePublishedCatalogues, ReadPublishedCatalogues
		);

		// What `SyncRendered` compares this frame's tree against, at the end of
		// the list for the reason it opens with. A resource, because a stamp
		// per world cannot be a static: two worlds exist and a world is ticked
		// by whichever worker claimed it.
		//
		// Registered rather than left to be minted from the compiler's
		// spelling, which is the failure `client::DrawList` was found in - and
		// with a writer that stores nothing, which is the other half. See the
		// pair above.
		ecs::Components::Register<RenderedSignature>(
			"scene.RenderedSignature", WriteRenderedSignatures, ReadRenderedSignatures
		);

		// **One per world, for the identical reason - and authored rather
		// than derived, so it is saved rather than reset.** A game that
		// picked a look for its screen expects a reopened place to still
		// have it.
		ecs::Components::Register<PostProcessing>(
			"scene.PostProcessing", WritePostProcessing, ReadPostProcessing
		);

		// **At the end, which is where a new component goes**, per the ordering
		// note above: an id decides column order and inserting one beside
		// `RigidBody` - where it belongs by subject - would reorder iteration
		// across the engine to no purpose.
		//
		// The generated form, for the reason `ecs.Hierarchy` uses it: the field
		// is an `Entity`, which is a directory index a snapshot restores exactly.
		ecs::Components::Register<NetworkOwner>("scene.NetworkOwner");

		// Appended, like every addition here: a component id is registration
		// order and it decides column order in a snapshot.
		ecs::Components::Register<AwakeWorld>("scene.AwakeWorld");

		// **Authored data, so it crosses.** Which part a portal leads to is a
		// fact about the scene rather than about whoever is looking at it -
		// the same side of the line `scene.SurfaceCamera` is already on, and
		// `replication::LocalToTheClient` names both.
		//
		// The generated form: the field is an `Entity`, which is a directory
		// index a snapshot and a replica both restore exactly.
		ecs::Components::Register<Portal>("scene.Portal");

		// **Derived data, so it does not.** A surface camera's frustum is fitted
		// to its pane *as seen from the local eye*, so the authority's answer is
		// wrong for every client watching - `client/Replicated.hpp` gives that
		// argument for the placement and this is the same fact one step on.
		// `LocalToTheClient` keeps it off the wire and `AimSurfaceCameras`
		// recomputes it on both ends.
		ecs::Components::Register<SurfaceLens>("scene.SurfaceLens");

		// **The three rows a character is held together by**, appended for the
		// ordering reason this list keeps repeating. All three are the generated
		// form: an `Entity` is a directory index a snapshot and a replica both
		// restore exactly, and a `CFrame` is its own object representation -
		// `scene.Pivot` is already registered on that argument.
		//
		// **All three cross the wire, and `CharacterLimb` is the one worth
		// stating.** A client poses its own limbs from the root it interpolated,
		// so the *offsets* have to arrive - and this row is what carries them.
		//
		// **It is also the tag that stops the limb transforms following them.**
		// Those were sent every tick and overwritten by `PoseCharacters` on the
		// frame they landed, which is what `D00115` filed: `replication` filtered
		// by component and a limb needed filtering by row. It does now -
		// `replication::DefaultReplicatedComponents` names this component as the
		// suppressor for `scene.Transform`, so an entity carrying one stops
		// paying for a frame the receiver computes. Nothing new had to be
		// declared, because an entity with a limb row already *means* an entity
		// whose transform is derived.
		ecs::Components::Register<Character>("scene.Character");
		ecs::Components::Register<CharacterLimb>("scene.CharacterLimb");
		ecs::Components::Register<PlayerCharacter>("scene.PlayerCharacter");

		// **It crosses, and it exists in order to cross.** A body goes through a
		// portal on the authority and the eye that follows it belongs to a
		// client, so this is the one fact about a crossing that has to reach the
		// other machine - see `scene::PortalTransit`. Appended for the ordering
		// reason this list keeps repeating, and the generated form: two scalars
		// with no name and no handle in them.
		ecs::Components::Register<PortalTransit>("scene.PortalTransit");

		// **No wire form, for `PreviousTransform`'s reason**: it is a record of
		// what a *viewer* has drawn, so a replica must keep its own and never be
		// handed the authority's. Giving it one would make every client agree
		// that a crossing had already been shown, which is precisely the state
		// in which nothing snaps.
		ecs::Components::Register<PortalTransitSeen>("scene.PortalTransitSeen");

		// **The three the player pipeline added, appended for this list's
		// standing reason.** Component ids are a dense counter and an archetype
		// is a sorted list of them, so inserting one anywhere but the end
		// changes the order every row in the engine is visited in.
		//
		// **`PlayersServiceComponent` and `PlayerIdentity` cross and
		// `PlayerRespawn` does not need to.** A client reads `Player.UserId` and
		// `DisplayName` off its own store, and `Players.MaxPlayers` is what a
		// game's own interface shows - where a respawn deadline is the
		// authority's bookkeeping and is recomputed there. All three are
		// registered because a component a snapshot meets and nothing registered
		// aborts the process; what differs is only what a game does with them.
		ecs::Components::Register<PlayersServiceComponent>("scene.PlayersService");
		ecs::Components::Register<PlayerIdentity>(
			"scene.PlayerIdentity", WritePlayerIdentities, ReadPlayerIdentities
		);
		ecs::Components::Register<PlayerRespawn>("scene.PlayerRespawn");
		ecs::Components::Register<CharacterChanges>(
			"scene.CharacterChanges", WriteCharacterChanges, ReadCharacterChanges
		);

		// **Registered rather than left to be minted, unlike `Gravity` and
		// `Sun` beside it.** `workspace.SurfaceBounces` is a declared property,
		// so the class table names this resource's component id while the tree
		// is being registered - and a type that reaches `Components::Of` before
		// an explicit name arrives keeps the compiler's spelling and aborts when
		// the real one turns up. Appended, for this list's standing reason.
		ecs::Components::Register<SurfaceBounces>("scene.SurfaceBounces");

		// `workspace.MaxSurfaces`, beside the depth and for the same reason: a
		// declared property has to resolve a component id, and a resource is
		// keyed by one like anything else.
		ecs::Components::Register<SurfaceLimit>("scene.SurfaceLimit");

		// **The three the team pipeline added, appended for this list's
		// standing reason**: a component id is registration order, an archetype
		// is a sorted list of ids, and inserting one anywhere but the end
		// reorders how every row in the engine is visited.
		//
		// All three are the generated form. A `Team` is three floats, a
		// `SpawnLocation` is three floats and two flags, and a `PlayerTeam` is
		// an `Entity` - a directory index a snapshot and a replica both restore
		// exactly, which is the argument `scene.PlayerCharacter` already makes.
		// No `core::Name` anywhere in them, so there is nothing to hand-write.
		//
		// **`scene.SpawnLocation` has to cross, and that is not obvious.** A
		// spawn pad is authored geometry, so the *part* would replicate anyway;
		// what this row carries is which side may use it, and a client that ran
		// `FindSpawn` - the studio does, through `PlayLink` - would put
		// everybody on the first neutral pad without it.
		ecs::Components::Register<Team>("scene.Team");
		ecs::Components::Register<PlayerTeam>("scene.PlayerTeam");
		ecs::Components::Register<SpawnLocation>("scene.SpawnLocation");

		// **What a `Tool` instance is, appended for this list's standing
		// reason**: a component id is registration order, an archetype is a
		// sorted list of ids, and inserting one anywhere but the end reorders how
		// every row in the engine is visited.
		//
		// The generated form - one `CFrame` and no name, which is the argument
		// `scene.Pivot` already makes.
		//
		// **It crosses, and what it decides is where a handle is drawn.** A
		// stowed tool reaches only its owner because it is under a `Player` and
		// `scene::PlayerOwning` says so; an equipped one is under a character in
		// `Workspace` and reaches everybody, and the `Grip` is what every one of
		// those clients poses the handle by. Nothing about that needed a rule of
		// its own - see `scene/Tools.hpp`.
		ecs::Components::Register<Tool>("scene.Tool");

		// Appended because component ids are registration order. This is authored
		// player state, so the generated scalar serializer is sufficient and the
		// default replication catalogue includes it with the other `scene.` rows.
		ecs::Components::Register<PlayerNetworkComponent>("scene.PlayerNetworkComponent");

		// --- the forward-declared storage --------------------------------------
		//
		// **Eight types nothing in the engine reads yet, registered here rather
		// than lazily**, and both halves of that sentence are decisions.
		//
		// *Registered*, because `Components::Seal()` is called by the client and
		// the server at start-up and a type that first reaches `Components::Of<T>`
		// after the seal aborts the process. A component declared and left to
		// register itself is a component that works in a unit test and takes down
		// every host the first time a game file carries one. That is the failure
		// `scene.Sun`, `scene.PortalProxy` and `physics.PoppercamState` were all
		// found in at v0.19.
		//
		// *Nothing reads them yet*, which is decision 16: a surface may ship
		// complete and frozen with its implementation deliberately unwired, and
		// the revisit condition is "never - it is a state, not a stage".
		// `docs/FUTURE_COMPONENTS.md` says what each of these gets wired to and in
		// what order.
		//
		// **Appended, like every addition here**, for the reason this list opens
		// with: a component id is registration order, an archetype is a sorted
		// list of ids, and inserting one anywhere but the end reorders how every
		// row in the engine is visited.

		// **A hand-written pair, because a `Skeleton` holds a name.** The rig's
		// name is what an animation clip is authored against, so it crosses a save
		// file and has to cross as text.
		ecs::Components::Register<Skeleton>("scene.Skeleton", WriteSkeletons, ReadSkeletons);

		// **The generated form, because a `Bone` is four `CFrame`s and two
		// indices.** No name and no handle, so the object representation is the
		// format - `scene.Attachment` is registered on the same argument, and a
		// bone is that argument with a chain on it.
		//
		// **It crosses, and `WorldFrame` crossing with it is deliberate.** The
		// resolved frame is derived on whichever machine draws, exactly as an
		// attachment's is, and it goes on the wire for the reason that one does:
		// `ResolveBones` runs in `PreRender`, so a client that cleared the field
		// on arrival would present a rig at the origin for one frame. A frame of a
		// character in the wrong place is more visible than the bytes.
		ecs::Components::Register<Bone>("scene.Bone");

		// **A hand-written pair, because a clip holds two names.**
		ecs::Components::Register<AnimationClip>(
			"scene.AnimationClip", WriteAnimationClips, ReadAnimationClips
		);

		// **Both generated, because neither holds a name.** An `Animator` is a
		// handle, a float and two flags; an `AnimationTrack` is a handle, five
		// floats, an enum and two flags. An `ecs::Entity` is a directory index a
		// snapshot and a replica both restore exactly, which is the argument
		// `scene.PlayerCharacter` already makes.
		//
		// **Both cross, and the play head crossing is the decision worth
		// stating.** `AnimationTrack::TimePosition` is advanced by whoever owns
		// the row, and `ecs::Store::SetProperty` already refuses a property write
		// in a replica - so the authority owns it exactly as it owns
		// `scene.Transform`, and a client predicting its own play head would be
		// predicting the same field rather than needing a second one. Nothing new
		// has to be stored for the v0.24 decision to go either way.
		ecs::Components::Register<Animator>("scene.Animator");
		ecs::Components::Register<AnimationTrack>("scene.AnimationTrack");

		// **A hand-written pair, because a ladder holds three mesh names.**
		//
		// **It crosses, because it is authored content and not a conclusion.**
		// Which four meshes a part has is what an author published; which of them
		// a frame draws is derived per view and is not stored anywhere, so there
		// is nothing here for a replica to disagree with. `scene.Visual` is on the
		// same side of that line for the same reason.
		ecs::Components::Register<LevelOfDetail>(
			"scene.LevelOfDetail", WriteLevelsOfDetail, ReadLevelsOfDetail
		);

		// **The generated form, because a `Constraint` is two handles, a `CFrame`,
		// six enum bytes and eight floats.** No name, so the object representation
		// is the format.
		//
		// **It crosses, and it has to.** A joint is authored scene content and the
		// solver runs on both ends: a client simulating its own replica with no
		// constraints would let a door swing free while the server held it shut.
		// The accumulated impulses a warm start needs are not here and must not
		// come to be - those are `physics::PhysicsWorld`'s, which does not cross
		// for `CollisionShapes`' reason.
		ecs::Components::Register<Constraint>("scene.Constraint");

		// **Both generated, and both cross.** An `Atmosphere` is two colours and
		// four floats and a `Clouds` is a colour, four floats and a flag; neither
		// holds a name or a handle.
		//
		// **Presentation state that crosses, which is not a contradiction.**
		// Decision 20 says a render graph may vary per platform and anything
		// reaching a simulation input may not - these reach no simulation input,
		// and what crosses is what the *author* wrote rather than what a machine
		// resolved. `scene.LightingService` is already on this side of the line
		// with the fog terms these sit beside; what stays local is the resolved
		// half, and the resolved half is `scene::WorldLighting`, which is not a
		// component at all.
		ecs::Components::Register<Atmosphere>("scene.Atmosphere");
		ecs::Components::Register<Clouds>("scene.Clouds");

		// **A resource, and a hand-written pair because it holds the generator's
		// name.**
		//
		// **The recipe crosses and the ground it makes never can.** A chunked
		// world is gigabytes; both ends run the same graph over the same seed and
		// get the same ground, which is decision 14's strict IEEE arithmetic doing
		// the work it exists to do. `CollisionShapes` makes the identical argument
		// at four fewer orders of magnitude: sending a conclusion instead of its
		// input hands an attacker the half they get to choose.
		//
		// Registered rather than left to be minted by the first `SetResource`,
		// which takes the compiler's spelling of the type and aborts once the
		// table is sealed.
		ecs::Components::Register<Terrain>("scene.Terrain", WriteTerrains, ReadTerrains);
	}

	void RegisterSceneClasses() {
		// The components first and then the whole tree, on the first call -
		// the same registration under the name a caller looks for rather than
		// a second one.
		EnsureClassTree();

		// The services, through the same door. A game file naming `Lighting`
		// has to resolve it, and a reader that depended on the studio having
		// registered these first would fail with "no class named 'Lighting'" on
		// a perfectly good file - which is exactly the failure
		// `game::RegisterGameClasses` exists to prevent for `Part`.
		(void)ServiceClass();

		// **`ShaderScript` through the same door, for the same reason.** It is
		// this module's class and it lives in its own file only because the
		// property that moves its revision does; a caller that had to know to
		// register it separately would be one that opened a game file naming
		// one and could not resolve it - and the studio's own insert menu is
		// built by walking everything registered under `Instance`, so a class
		// nothing registered is a class nobody can create.
		(void)ShaderScriptClass();
		(void)EditableMeshClass();
		(void)EditableImageClass();
	}
}
