#include <engine/core/Bytes.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/scene/ActiveCamera.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/scene/SurfaceTable.hpp>
#include <engine/scene/Visibility.hpp>
#include <engine/scene/Wire.hpp>

#include <cstddef>
#include <cstdint>

namespace engine::scene {

	const core::Name &DefaultMaterial() {
		// **`Plastic` spelled here and registered as an enum member in
		// `Part.cpp`, which is two places holding one string.** They are not
		// collapsible in the direction that would help: the enum list is
		// registered when the class tree is built, and a `Visual` can be default
		// constructed before that has happened — a replica's column grows from a
		// wire delta, and nothing on that path builds a class tree. Reading the
		// enum's first member instead would therefore return an invalid name on
		// exactly the path this default exists to serve.
		//
		// `scene/tests/Components.cpp` asserts the two agree, which is the check
		// that makes the duplication safe rather than merely stated.
		static const core::Name name("Plastic");
		return name;
	}

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

		void WriteVisuals(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *visuals = static_cast<const Visual *>(source);
			for (size_t index = 0; index < count; index++) {
				const Visual &visual = visuals[index];
				writer.WriteFloat(visual.Tint.R);
				writer.WriteFloat(visual.Tint.G);
				writer.WriteFloat(visual.Tint.B);
				writer.WriteName(visual.Mesh);
				writer.WriteName(visual.Material);
				writer.WriteBool(visual.Visible);

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
				visual.Material = reader.ReadName();
				visual.Visible = reader.ReadBool();
				visual.Transparency = reader.ReadFloat();
				visual.Surface = reader.ReadInt8();
				visual.CastShadow = reader.ReadBool();
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
		ecs::Components::Register<ecs::Hierarchy>("ecs.Hierarchy");

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
		ecs::Components::Register<Visual>("scene.Visual", WriteVisuals, ReadVisuals);
		ecs::Components::Register<SurfaceCamera>("scene.SurfaceCamera");
		ecs::Components::Register<Camera>("scene.Camera");
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
		ecs::Components::Register<ActiveCamera>("scene.ActiveCamera");
		ecs::Components::Register<WorldBounds>("scene.WorldBounds");
	}

	void RegisterSceneClasses() {
		// `PartClass` registers the components first and the whole tree on its
		// first call, so this is the same registration under the name a caller
		// looks for rather than a second one.
		(void)PartClass();

		// The services, through the same door. A game file naming `Lighting`
		// has to resolve it, and a reader that depended on the studio having
		// registered these first would fail with "no class named 'Lighting'" on
		// a perfectly good file — which is exactly the failure
		// `game::RegisterGameClasses` exists to prevent for `Part`.
		(void)ServiceClass();
	}
}
