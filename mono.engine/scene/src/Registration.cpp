#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Materials.hpp>
#include <engine/scene/MeshCatalogue.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/PublishedCatalogue.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/Shaders.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/scene/Tagging.hpp>
#include <engine/scene/TextureCatalogue.hpp>
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
		// number — which is not a corrupt file, it is a file that loads and is
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
				// both silently reset to their defaults on every load —
				// invisible for a part that was opaque anyway, and a glass pane
				// that turned solid the first time a world was saved and
				// reopened. `mono.client/tests/Presentation.cpp` is the check
				// that would have caught it, and now does.
				writer.WriteFloat(visual.Transparency);
				writer.WriteInt8(visual.Surface);

				// Added at v0.7 beside the shadow pass that reads it, and
				// written here in the same breath rather than a release later —
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
				visual.Surface = reader.ReadInt8();
				visual.CastShadow = reader.ReadBool();
				visual.Locked = reader.ReadBool();
			}
		}

		// **A `RenderedSignature` is derived state, and its serialisation says
		// so by writing nothing.**
		//
		// It is not merely redundant to save — it is dangerous. The stamp says
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
				// the whole directory — so a handle kept across it names
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
			}
		}

		// **A hand-written pair, because it holds a name**, and every field is
		// written the day it is added rather than a release later — the lesson
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
				// alternative reads as tidier — a loaded world starts silent —
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
				appearances[index].Mode = mode <= static_cast<uint8_t>(AlphaMode::Blend)
											  ? static_cast<AlphaMode>(mode)
											  : AlphaMode::Opaque;
			}
		}

		// **The tag names, and they have to travel with the masks.** A `Tags`
		// component is a bare integer whose bits mean whatever this table says
		// they mean, so a world restored with the masks and without the table
		// would have every tagged object in a group with no name — and a surface
		// camera filtering by name would match nothing, silently.
		void WriteTagTables(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *tables = static_cast<const TagTable *>(source);
			for (size_t index = 0; index < count; index++) {
				writer.WriteUInt32(static_cast<uint32_t>(tables[index].Names.size()));

				// In registration order, because the index *is* the bit. Sorting
				// here would renumber every mask already stored on a row —
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
				// order — the sequence the scene registered materials in — so
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
		// and this is the only place either half of the engine says so — a
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
		// the compiler's spelling — which is unusable on a wire, because the two
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
		// authority's indices — `Store::SetAdoptOnly` exists to guarantee it —
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
		// instance components itself now — see `RegisterInstanceComponents`.

		// Added at the end of this list rather than beside `ServiceComponent`,
		// per the ordering note above: a component id decides column order, and
		// inserting one in the middle reorders iteration across the engine.
		ecs::Components::Register<Transform>("scene.Transform", TransformWire());
		ecs::Components::Register<PreviousTransform>("scene.PreviousTransform");
		ecs::Components::Register<Bounds>("scene.Bounds");
		ecs::Components::Register<Motion>("scene.Motion", MotionWire());
		ecs::Components::Register<RigidBody>("scene.RigidBody");
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
		// integer and no name — the *names* are in the `TagTable` resource,
		// which is serialised beside it, so the mask and the meanings of its
		// bits travel together or not at all.
		ecs::Components::Register<Tags>("scene.Tags");
		ecs::Components::Register<SurfaceCamera>("scene.SurfaceCamera");

		// **A written pair rather than the generated one**, because the type
		// holds a `std::string` and is therefore not trivially copyable —
		// `DescribeType` offers raw serialisation only for a type that is.
		ecs::Components::Register<TextContent>("scene.TextContent", WriteTexts, ReadTexts);
		ecs::Components::Register<Camera>("scene.Camera");

		// A name again, so a hand-written pair again. See `WriteSounds`.
		ecs::Components::Register<Sound>("scene.Sound", WriteSounds, ReadSounds);

		// **Both generated, because neither holds a `core::Name`.** The rule this
		// file opens with is about names and only about names: a name's id is a
		// counter this process assigned, so it is written as text. An
		// `Attachment` is two `CFrame`s and a `Light` is a colour, four floats and
		// three bytes of enum and flags — all of which mean the same thing in
		// every process, so the object representation is the format and the
		// generated pair is correct.
		//
		// **The derived half of an `Attachment` crosses too**, and that is worth a
		// sentence rather than a shrug: `WorldFrame` is a cache with one writer,
		// and writing a cache into a snapshot is normally the thing rule 2
		// refuses. It goes in because the alternative is a restored world whose
		// beams draw at the origin for one frame — `ResolveAttachments` runs in
		// `PreRender`, so a load that cleared it would present before it was
		// filled. One frame of a beam in the wrong place is more visible than
		// thirty-two bytes an entity.
		// **Generated, because a `Pivot` is one `CFrame`** — no name, so the
		// object representation is the format. Registered beside `Attachment`
		// for the same reason it is: both are placements relative to something
		// else.
		ecs::Components::Register<Pivot>("scene.Pivot");

		ecs::Components::Register<Attachment>("scene.Attachment");
		ecs::Components::Register<Light>("scene.Light");

		// **Generated, because a `Humanoid` is nine floats and four flags.** No
		// name, so the object representation is the format and there is nothing
		// to hand-write — which is the desirable case and the one most components
		// here are in.
		ecs::Components::Register<Humanoid>("scene.Humanoid");

		ecs::Components::Register<QuickHash>("scene.QuickHash");

		// The service fixtures, added at the end because that is the rule this
		// list opens with: registration order decides component ids, ids decide
		// the order archetypes iterate their columns, and inserting one of
		// these above `Transform` would change the order every row in the
		// engine is visited in. Neither carries a wire form — a service is
		// authored content that a snapshot moves, not per-tick state a delta
		// does.
		ecs::Components::Register<TransientComponent>("scene.Transient");
		ecs::Components::Register<ServiceComponent>("scene.Service");
		ecs::Components::Register<LightingServiceComponent>("scene.LightingService");

		// **Who this host is looking through, and only a client has one.** A
		// resource, so it is carried by a snapshot and covered by the affinity
		// check like everything else a world holds — and named explicitly
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
		// `Store::SetResource` — under the compiler's spelling of the type, and
		// aborting outright once the table is sealed.
		ecs::Components::Register<SurfaceTable>("scene.SurfaceTable", WriteSurfaceTables, ReadSurfaceTables);
		ecs::Components::Register<TagTable>("scene.TagTable", WriteTagTables, ReadTagTables);
		ecs::Components::Register<ActiveCamera>("scene.ActiveCamera");

		// **`InputState` crosses and `CameraController` crosses**, which is worth
		// a sentence because one of them looks like it should not. Input is
		// per-frame state and writing it into a save file sounds like storing an
		// answer that is recomputed — and it is, except that a *recording* is the
		// case that matters: `just replay-check` replays a world from a snapshot
		// and its input stream, and a snapshot whose input state was cleared would
		// replay a character that never moved.
		//
		// The camera controller is authored state — a mode, a zoom, a sensitivity
		// — and crosses for the ordinary reason.
		ecs::Components::Register<InputState>("scene.InputState");
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
		// spelling, which is the failure `client::DrawList` was found in — and
		// with a writer that stores nothing, which is the other half. See the
		// pair above.
		ecs::Components::Register<RenderedSignature>(
			"scene.RenderedSignature", WriteRenderedSignatures, ReadRenderedSignatures
		);

		// **At the end, which is where a new component goes**, per the ordering
		// note above: an id decides column order and inserting one beside
		// `RigidBody` — where it belongs by subject — would reorder iteration
		// across the engine to no purpose.
		//
		// The generated form, for the reason `ecs.Hierarchy` uses it: the field
		// is an `Entity`, which is a directory index a snapshot restores exactly.
		ecs::Components::Register<NetworkOwner>("scene.NetworkOwner");

		// Appended, like every addition here: a component id is registration
		// order and it decides column order in a snapshot.
		ecs::Components::Register<AwakeWorld>("scene.AwakeWorld");

		// **Authored data, so it crosses.** Which part a portal leads to is a
		// fact about the scene rather than about whoever is looking at it —
		// the same side of the line `scene.SurfaceCamera` is already on, and
		// `replication::LocalToTheClient` names both.
		//
		// The generated form: the field is an `Entity`, which is a directory
		// index a snapshot and a replica both restore exactly.
		ecs::Components::Register<Portal>("scene.Portal");

		// **Derived data, so it does not.** A surface camera's frustum is fitted
		// to its pane *as seen from the local eye*, so the authority's answer is
		// wrong for every client watching — `client/Replicated.hpp` gives that
		// argument for the placement and this is the same fact one step on.
		// `LocalToTheClient` keeps it off the wire and `AimSurfaceCameras`
		// recomputes it on both ends.
		ecs::Components::Register<SurfaceLens>("scene.SurfaceLens");

		// **The three rows a character is held together by**, appended for the
		// ordering reason this list keeps repeating. All three are the generated
		// form: an `Entity` is a directory index a snapshot and a replica both
		// restore exactly, and a `CFrame` is its own object representation —
		// `scene.Pivot` is already registered on that argument.
		//
		// **All three cross the wire, and `CharacterLimb` is the one worth
		// stating.** A client poses its own limbs from the root it interpolated,
		// so the *offsets* have to arrive — and this row is what carries them.
		//
		// **It is also the tag that stops the limb transforms following them.**
		// Those were sent every tick and overwritten by `PoseCharacters` on the
		// frame they landed, which is what `D00115` filed: `replication` filtered
		// by component and a limb needed filtering by row. It does now —
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
		// other machine — see `scene::PortalTransit`. Appended for the ordering
		// reason this list keeps repeating, and the generated form: two scalars
		// with no name and no handle in them.
		ecs::Components::Register<PortalTransit>("scene.PortalTransit");
	}

	void RegisterSceneClasses() {
		// The components first and then the whole tree, on the first call —
		// the same registration under the name a caller looks for rather than
		// a second one.
		EnsureClassTree();

		// The services, through the same door. A game file naming `Lighting`
		// has to resolve it, and a reader that depended on the studio having
		// registered these first would fail with "no class named 'Lighting'" on
		// a perfectly good file — which is exactly the failure
		// `game::RegisterGameClasses` exists to prevent for `Part`.
		(void)ServiceClass();

		// **`ShaderScript` through the same door, for the same reason.** It is
		// this module's class and it lives in its own file only because the
		// property that moves its revision does; a caller that had to know to
		// register it separately would be one that opened a game file naming
		// one and could not resolve it — and the studio's own insert menu is
		// built by walking everything registered under `Instance`, so a class
		// nothing registered is a class nobody can create.
		(void)ShaderScriptClass();
	}
}
